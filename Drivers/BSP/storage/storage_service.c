#include "storage_service.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"
#include "fatfs.h"
#include "device_protocol.h"

#define STORAGE_QUEUE_DEPTH       8U
#define STORAGE_READ_QUEUE_DEPTH  2U
#define STORAGE_RETRY_DELAY_MS    2000U
#define STORAGE_RECORD_FILE       "records.bin"
#define STORAGE_PROGRESS_FILE     "upload.idx"

/* upload.idx使用两个固定槽位轮换写入，单槽损坏时仍可恢复上一份有效进度。 */
#define STORAGE_PROGRESS_MAGIC        0x55504C44UL /* ASCII: UPLD */
#define STORAGE_PROGRESS_SLOT_SIZE    16U
#define STORAGE_PROGRESS_SLOT_COUNT   2U
#define STORAGE_PROGRESS_FILE_SIZE    (STORAGE_PROGRESS_SLOT_SIZE * STORAGE_PROGRESS_SLOT_COUNT)

typedef enum
{
    STORAGE_COMMAND_APPEND = 0,
    STORAGE_COMMAND_VERIFY_ALL,
    STORAGE_COMMAND_READ_NEXT_PENDING,
    STORAGE_COMMAND_SET_UPLOAD_OFFSET
} StorageCommandType_t;

/*
 * 文件中的每条记录格式：
 * [0~1] 完整协议帧长度（大端） | [2~] 完整协议帧（包含帧头和CRC）。
 * 保存完整帧后，后续历史补传可以直接恢复原设备ID、序号、时间戳和CRC。
 */
typedef struct
{
    StorageCommandType_t type; /* StorageTask需要执行的操作类型。 */
    uint16_t length;           /* APPEND命令中完整协议帧的字节数。 */
    uint32_t offset;           /* SET_UPLOAD_OFFSET命令中的目标文件偏移。 */
    uint8_t frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* APPEND命令携带的完整编码帧。 */
} StorageCommand_t;

static osMessageQueueId_t storage_queue;       /* 其他任务向StorageTask投递操作的唯一入口。 */
static osMessageQueueId_t storage_read_queue;  /* StorageTask向NetworkTask返回读取结果。 */
static volatile uint32_t upload_offset_cache;  /* 最近一次成功加载或落盘的上传偏移。 */
static volatile uint8_t upload_offset_ready;   /* 1表示缓存已经完成初始化，可以被读取。 */
static uint32_t upload_progress_generation;    /* 双槽进度记录的当前有效代数。 */
static uint32_t pending_history_next_offset;   /* 最近一条校验成功历史记录的结束偏移。 */
static uint8_t pending_history_offset_valid;   /* 1表示上述偏移可由匹配ACK推进。 */

static uint16_t StorageService_ReadU16BE(const uint8_t *data);
static FRESULT StorageService_SaveUploadProgress(FIL *record_file, uint32_t offset);

static void StorageService_PublishReadResult(const StorageReadResult_t *read_result)
{
    if ((storage_read_queue == NULL) ||
        (osMessageQueuePut(storage_read_queue, read_result, 0U, 0U) != osOK))
    {
        /* 结果队列满时不阻塞StorageTask，调用者可以重新提交读取请求。 */
        printf("STORAGE READ RESULT QUEUE FULL\r\n");
    }
}

static FRESULT StorageService_ReadNextPendingRecord(FIL *file)
{
    DeviceProtocol_Parser_t parser; /* 使用现有协议解析器校验记录格式和CRC。 */
    DeviceProtocol_Frame_t frame;  /* 保存读取成功后的设备ID、序号和时间戳。 */
    FRESULT result;                /* 当前定位或读取操作的FatFs结果。 */
    FSIZE_t file_size = f_size(file); /* 读取开始时records.bin的总长度。 */
    FSIZE_t current_offset;        /* upload.idx指向的当前待上传记录位置。 */
    FSIZE_t next_offset;           /* 当前记录结束位置，也是ACK后的新进度。 */
    UINT bytes_read;               /* f_read实际返回的字节数。 */
    uint8_t length_bytes[2];       /* 磁盘记录开头的2字节大端长度。 */
    uint8_t encoded_frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 读取出的完整协议帧。 */
    uint16_t frame_length;         /* 长度头声明的完整协议帧长度。 */
    uint16_t index;                /* 逐字节送入协议解析器的数组下标。 */
    uint8_t frame_valid = 0U;      /* 1表示整条记录通过格式和CRC检查。 */
    StorageReadResult_t read_result; /* 返回给NetworkTask的异步读取结果。 */

    memset(&read_result, 0, sizeof(read_result));

    if (upload_offset_ready == 0U)
    {
        printf("STORAGE PENDING READ WAIT PROGRESS\r\n");
        return FR_NOT_READY;
    }

    current_offset = (FSIZE_t)upload_offset_cache;
    read_result.current_offset = (uint32_t)current_offset;
    if (current_offset == file_size)
    {
        pending_history_offset_valid = 0U;
        read_result.status = STORAGE_READ_END;
        read_result.next_offset = (uint32_t)current_offset;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING END: OFFSET=%lu\r\n", (unsigned long)current_offset);
        return FR_OK;
    }

    if (current_offset > file_size)
    {
        read_result.status = STORAGE_READ_CORRUPT;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING OFFSET OUT OF RANGE: OFFSET=%lu SIZE=%lu\r\n",  (unsigned long)current_offset, (unsigned long)file_size);
        return FR_OK;
    }

    /* 定位到upload.idx记录的位置，只读取这一条，不改变上传进度。 */
    result = f_lseek(file, current_offset);
    if (result != FR_OK)
    {
        return result;
    }

    /* 先读取2字节长度头，用它确定随后需要读取多少协议帧数据。 */
    result = f_read(file, length_bytes, sizeof(length_bytes), &bytes_read);
    if (result != FR_OK)
    {
        return result;
    }
    if (bytes_read != sizeof(length_bytes))
    {
        read_result.status = STORAGE_READ_CORRUPT;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING TRUNCATED HEADER: OFFSET=%lu\r\n",
               (unsigned long)current_offset);
        (void)f_lseek(file, file_size);
        return FR_OK;
    }

    frame_length = StorageService_ReadU16BE(length_bytes);
    if ((frame_length < (DEVICE_PROTOCOL_FIXED_SIZE + DEVICE_PROTOCOL_CRC_SIZE)) ||
        (frame_length > DEVICE_PROTOCOL_MAX_FRAME_SIZE))
    {
        read_result.status = STORAGE_READ_CORRUPT;
        read_result.frame_length = frame_length;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING BAD LENGTH: OFFSET=%lu LEN=%u\r\n",
               (unsigned long)current_offset, (unsigned int)frame_length);
        (void)f_lseek(file, file_size);
        return FR_OK;
    }

    next_offset = current_offset + 2U + frame_length;
    read_result.next_offset = (uint32_t)next_offset;
    read_result.frame_length = frame_length;
    if (next_offset > file_size)
    {
        read_result.status = STORAGE_READ_CORRUPT;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING TRUNCATED FRAME: OFFSET=%lu LEN=%u SIZE=%lu\r\n",
               (unsigned long)current_offset,
               (unsigned int)frame_length,
               (unsigned long)file_size);
        (void)f_lseek(file, file_size);
        return FR_OK;
    }

    /* 按长度头读取完整协议帧，短读仍视为文件尾部不完整记录。 */
    result = f_read(file, encoded_frame, frame_length, &bytes_read);
    if (result != FR_OK)
    {
        return result;
    }
    if (bytes_read != frame_length)
    {
        read_result.status = STORAGE_READ_CORRUPT;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING SHORT READ: OFFSET=%lu READ=%u LEN=%u\r\n",
               (unsigned long)current_offset,
               (unsigned int)bytes_read,
               (unsigned int)frame_length);
        (void)f_lseek(file, file_size);
        return FR_OK;
    }

    DeviceProtocol_ParserReset(&parser);
    for (index = 0U; index < frame_length; index++)
    {
        if (DeviceProtocol_ParserFeed(&parser, encoded_frame[index], &frame) != 0U)
        {
            /* 一条磁盘记录只能包含一张完整协议帧，并且必须在最后一个字节结束。 */
            frame_valid = (index == (uint16_t)(frame_length - 1U)) ? 1U : 0U;
            break;
        }
    }

    if (frame_valid == 0U)
    {
        read_result.status = STORAGE_READ_CORRUPT;
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING CRC/FORMAT ERROR: OFFSET=%lu LEN=%u\r\n",
               (unsigned long)current_offset, (unsigned int)frame_length);
    }
    else
    {
        /* 只有这次实际读取并通过CRC的next_offset，才可以在ACK后写入upload.idx。 */
        pending_history_next_offset = (uint32_t)next_offset;
        pending_history_offset_valid = 1U;
        read_result.status = STORAGE_READ_OK;
        read_result.device_id = frame.device_id;
        read_result.sequence = frame.sequence;
        read_result.timestamp = frame.timestamp;
        memcpy(read_result.frame, encoded_frame, frame_length);
        StorageService_PublishReadResult(&read_result);
        printf("STORAGE PENDING OK: OFFSET=%lu NEXT=%lu ID=%08lX SEQ=%lu TIME=%lu LEN=%u\r\n",
               (unsigned long)current_offset,
               (unsigned long)next_offset,
               (unsigned long)frame.device_id,
               (unsigned long)frame.sequence,
               (unsigned long)frame.timestamp,
               (unsigned int)frame_length);
    }

    /* 恢复到文件末尾，保证之后的APPEND命令继续追加而不是覆盖记录。 */
    return f_lseek(file, file_size);
}

static uint16_t StorageService_ReadU16BE(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t StorageService_ReadU32BE(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           data[3];
}

static void StorageService_WriteU16BE(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static void StorageService_WriteU32BE(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

/* offset必须正好位于文件开头、某条记录之后或文件末尾，不能落在帧内部。 */
static uint8_t StorageService_IsRecordBoundary(FIL *record_file, FSIZE_t offset)
{
    FRESULT result;                         /* 保存本轮FatFs操作结果。 */
    FSIZE_t file_size = f_size(record_file);/* records.bin当前总长度。 */
    FSIZE_t position = 0U;                  /* 从文件头逐条扫描得到的记录边界。 */
    UINT bytes_read;                        /* f_read实际返回的字节数。 */
    uint8_t length_bytes[2];                /* 磁盘记录前两个字节的大端长度头。 */
    uint16_t frame_length;                  /* 当前记录内完整协议帧长度。 */

    if (offset > file_size)
    {
        return 0U;
    }

    /*移动光标到文件开头*/
    result = f_lseek(record_file, 0U);
    while ((result == FR_OK) && (position < offset))
    {
        /*读取bin前两个字节的长度文件*/
        result = f_read(record_file, length_bytes, sizeof(length_bytes), &bytes_read);
        if ((result != FR_OK) || (bytes_read != sizeof(length_bytes)))
        {
            break;
        }

        /*解析数据长度*/
        frame_length = StorageService_ReadU16BE(length_bytes);

        /*判断数据长度是否在合理的范围*/
        if ((frame_length < (DEVICE_PROTOCOL_FIXED_SIZE + DEVICE_PROTOCOL_CRC_SIZE)) ||
            (frame_length > DEVICE_PROTOCOL_MAX_FRAME_SIZE))
        {
            result = FR_INT_ERR;
            break;
        }

        /*都指针跳到第二条数据的开头*/
        position += (FSIZE_t)(2U + frame_length);

        /*如果读指针大于文件大小，错误*/
        if (position > file_size)
        {
            result = FR_INT_ERR;
            break;
        }
        result = f_lseek(record_file, position);
    }

    /* 无论检查结果如何，都恢复追加位置，避免后续覆盖已有记录。 */
    (void)f_lseek(record_file, file_size);
    return ((result == FR_OK) && (position == offset)) ? 1U : 0U;
}

static uint8_t StorageService_DecodeProgressSlot(const uint8_t *slot,
                                                 uint32_t *generation,
                                                 uint32_t *offset)
{
    uint16_t stored_crc;     /* upload.idx槽位末尾保存的CRC。 */
    uint16_t calculated_crc; /* 根据槽位前14字节重新计算的CRC。 */


    /*判断第一位是不是等于魔术字，或者第四位的版本*/
    if ((StorageService_ReadU32BE(&slot[0]) != STORAGE_PROGRESS_MAGIC) || (slot[4] != 1U))
    {
        return 0U;
    }

    /*获取crc*/
    stored_crc = StorageService_ReadU16BE(&slot[14]);

    /*计算数据的crc*/
    calculated_crc = DeviceProtocol_CalculateCrc16(slot, 14U);

    /*核实数据自带的crc*/
    if (stored_crc != calculated_crc)
    {
        return 0U;
    }

    /*次数*/
    *generation = StorageService_ReadU32BE(&slot[6]);

    /*获取偏移数据*/
    *offset = StorageService_ReadU32BE(&slot[10]);
    return 1U;
}

static FRESULT StorageService_LoadUploadProgress(FIL *record_file)
{
    FIL progress_file;      /* upload.idx文件对象，只能在StorageTask中使用。 */
    FRESULT result;         /* 打开、读取和关闭进度文件的结果。 */
    UINT bytes_read;        /* 实际从upload.idx读取的字节数。 */
    uint8_t slots[STORAGE_PROGRESS_FILE_SIZE]; /* 两个进度槽位的原始内容。 */
    uint8_t slot_valid;     /* 当前槽位是否通过Magic、版本和CRC检查。 */
    uint8_t found = 0U;     /* 是否至少找到一个格式有效的槽位。 */
    uint32_t generation;    /* 当前检查槽位的写入代数。 */
    uint32_t offset;        /* 当前检查槽位保存的上传偏移。 */
    uint32_t best_generation = 0U; /* 两个有效槽位中最新的代数。 */
    uint32_t best_offset = 0U;     /* 最新有效槽位对应的上传偏移。 */
    uint8_t slot_index;     /* 正在检查的槽位下标，取值0或1。 */

    /* 只读打开已有进度文件；文件不存在属于首次启动，不作为错误。 */
    result = f_open(&progress_file, STORAGE_PROGRESS_FILE, FA_READ | FA_OPEN_EXISTING);
    if (result == FR_NO_FILE)
    {
        upload_progress_generation = 0U;
        upload_offset_cache = 0U;
        upload_offset_ready = 0U;
        printf("UPLOAD PROGRESS EMPTY: OFFSET=0\r\n");
        /* 首次启动立即建立进度文件，便于掉电重启和拔卡检查。 */
        return StorageService_SaveUploadProgress(record_file, 0U);
    }
    if (result != FR_OK)
    {
        return result;
    }

    memset(slots, 0, sizeof(slots));
    /* 一次读取两个固定槽位，短文件中未读到的部分保持为0并自然校验失败。 */
    result = f_read(&progress_file, slots, sizeof(slots), &bytes_read);
    (void)f_close(&progress_file);
    if (result != FR_OK)
    {
        return result;
    }

    for (slot_index = 0U; slot_index < STORAGE_PROGRESS_SLOT_COUNT; slot_index++)
    {
        slot_valid = StorageService_DecodeProgressSlot( &slots[slot_index * STORAGE_PROGRESS_SLOT_SIZE], 
                                                        &generation, &offset);

        if ((slot_valid != 0U) && ((found == 0U) || ((int32_t)(generation - best_generation) > 0)))
        {
            found = 1U;

            /*最新的版本*/
            best_generation = generation;
            /*最新的偏移*/
            best_offset = offset;
        }
    }

    if ((found == 0U) || (StorageService_IsRecordBoundary(record_file, (FSIZE_t)best_offset) == 0U))
    {
        /* 进度文件损坏或越界时从头开始，宁可重复上传也不能跳过数据。 */
        upload_progress_generation = (found != 0U) ? best_generation : 0U;
        upload_offset_cache = 0U;
        upload_offset_ready = 0U;
        printf("UPLOAD PROGRESS INVALID: FALLBACK OFFSET=0\r\n");
        /* 用新的有效槽自愈；旧槽仍保留，写入失败时不会破坏records.bin。 */
        return StorageService_SaveUploadProgress(record_file, 0U);
    }

    upload_progress_generation = best_generation;
    upload_offset_cache = best_offset;
    upload_offset_ready = 1U;
    printf("UPLOAD PROGRESS LOAD: GEN=%lu OFFSET=%lu\r\n", (unsigned long)best_generation, (unsigned long)best_offset);
    
    return FR_OK;
}

static FRESULT StorageService_SaveUploadProgress(FIL *record_file, uint32_t offset)
{
    FIL progress_file;      /* 本次更新upload.idx使用的文件对象。 */
    FRESULT result;         /* 记录打开、定位、写入和同步的结果。 */
    UINT written;           /* f_write实际写入的字节数。 */
    uint8_t slot[STORAGE_PROGRESS_SLOT_SIZE]; /* 即将写入的新进度槽位。 */
    uint32_t generation;    /* 在当前有效代数基础上递增的新代数。 */
    uint32_t slot_index;    /* 本次轮换写入的槽位下标。 */
    FSIZE_t slot_position;  /* 当前槽位在upload.idx中的起始偏移。 */

    if (((offset == 0U) &&
         (StorageService_IsRecordBoundary(record_file, (FSIZE_t)offset) == 0U)) ||
        ((offset != 0U) &&
         ((pending_history_offset_valid == 0U) ||
          (offset != pending_history_next_offset) ||
          ((FSIZE_t)offset > f_size(record_file)))))
    {
        printf("UPLOAD PROGRESS REJECTED: OFFSET=%lu NOT RECORD BOUNDARY\r\n",
               (unsigned long)offset);
        return FR_INVALID_PARAMETER;
    }

    /*记录第几次进入*/
    generation = upload_progress_generation + 1U;

    /*用取余操作来进行交替写入。*/
    slot_index = generation % STORAGE_PROGRESS_SLOT_COUNT;

    /*0--前16位，1--后16位*/
    slot_position = (FSIZE_t)(slot_index * STORAGE_PROGRESS_SLOT_SIZE);

    memset(slot, 0, sizeof(slot));

    /*准备要写入文件的数据*/
    StorageService_WriteU32BE(&slot[0], STORAGE_PROGRESS_MAGIC);/*写入魔术字*/
    slot[4] = 1U; /* 进度文件格式版本。 */
    slot[5] = (uint8_t)slot_index;/*索引*/
    StorageService_WriteU32BE(&slot[6], generation);
    StorageService_WriteU32BE(&slot[10], offset);
    StorageService_WriteU16BE(&slot[14],DeviceProtocol_CalculateCrc16(slot, 14U));/*写入crc*/

    /* 读写方式打开；首次运行时由FA_OPEN_ALWAYS负责创建upload.idx。 */
    result = f_open(&progress_file, STORAGE_PROGRESS_FILE, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
    if (result != FR_OK)
    {
        return result;
    }

    /* 定位到非当前槽位，保留上一份有效进度作为掉电回退。 */
    result = f_lseek(&progress_file, slot_position);
    if (result == FR_OK)
    {
        /* 整个固定槽位一次写入，实际写入长度必须与槽位长度一致。 */
        result = f_write(&progress_file, slot, sizeof(slot), &written);
        if ((result == FR_OK) && (written != sizeof(slot)))
        {
            result = FR_DISK_ERR;
        }
    }
    if (result == FR_OK)
    {
        /* 先同步到SD卡，再发布新的内存缓存，避免把未落盘进度当成成功。 */
        result = f_sync(&progress_file);
    }
    (void)f_close(&progress_file);

    if (result == FR_OK)
    {
        /* 只有数据真正同步到SD卡后，才对外发布新的缓存值。 */
        upload_progress_generation = generation;
        upload_offset_cache = offset;
        upload_offset_ready = 1U;
        if ((pending_history_offset_valid != 0U) &&
            (offset == pending_history_next_offset))
        {
            pending_history_offset_valid = 0U;
        }
        printf("UPLOAD PROGRESS SAVE: GEN=%lu OFFSET=%lu\r\n", (unsigned long)generation, (unsigned long)offset);
    }
    return result;
}

static FRESULT StorageService_VerifyAllRecords(FIL *file)
{
    DeviceProtocol_Parser_t parser; /* 用现有协议解析器验证帧格式与CRC。 */
    DeviceProtocol_Frame_t frame;  /* 保存当前成功解析出的协议字段。 */
    FRESULT result;                /* 当前FatFs读取或定位结果。 */
    FSIZE_t file_size;             /* 开始校验时records.bin的总长度。 */
    FSIZE_t record_offset;         /* 当前记录长度头在文件中的位置。 */
    UINT bytes_read;               /* f_read实际读取的字节数。 */
    uint8_t length_bytes[2];       /* 当前记录的大端长度头。 */
    uint8_t encoded_frame[DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 当前完整编码帧。 */
    uint16_t frame_length;         /* 长度头声明的协议帧长度。 */
    uint16_t index;                /* 向协议解析器逐字节送入数据的下标。 */
    uint32_t record_count = 0U;    /* 已通过格式和CRC校验的记录数量。 */

    /*计算文件的大小*/
    file_size = f_size(file);

    /*移动光标的文件的开始*/
    result = f_lseek(file, 0U);
    if (result != FR_OK)
    {
        return result;
    }

    printf("STORAGE VERIFY START: SIZE=%lu\r\n", (unsigned long)file_size);

    while (f_tell(file) < file_size)
    {
        record_offset = f_tell(file);

        /*读取前两个记录长度的字节*/
        /* 先读取2字节长度头，只有长度合法才继续读取后面的完整协议帧。 */
        result = f_read(file, length_bytes, sizeof(length_bytes), &bytes_read);
        if (result != FR_OK)
        {
            return result;
        }

        if (bytes_read != sizeof(length_bytes))
        {
            printf("STORAGE RECORD TRUNCATED: OFFSET=%lu HEADER=%u/2\r\n",
                   (unsigned long)record_offset, (unsigned int)bytes_read);
            break;
        }

        /*解析长度*/
        frame_length = (uint16_t)(((uint16_t)length_bytes[0] << 8U) |  length_bytes[1]);

        /*判断解析出来的长度是否在合理的范围*/
        if ((frame_length < (DEVICE_PROTOCOL_FIXED_SIZE + DEVICE_PROTOCOL_CRC_SIZE)) ||
            (frame_length > DEVICE_PROTOCOL_MAX_FRAME_SIZE))
        {
            /* 长度字段损坏后无法可靠寻找下一条边界，因此停在当前记录。 */
            printf("STORAGE RECORD BAD LENGTH: OFFSET=%lu LEN=%u\r\n",
                    (unsigned long)record_offset, (unsigned int)frame_length);
            break;
        }

        /*从sd卡里面读取数据到encoded_frame数组里面去*/
        /* 按长度头读取一整帧，短读说明文件尾存在不完整记录。 */
        result = f_read(file, encoded_frame, frame_length, &bytes_read);
        if (result != FR_OK)
        {
            return result;
        }

        if (bytes_read != frame_length)
        {
            printf("STORAGE RECORD TRUNCATED: OFFSET=%lu FRAME=%u/%u\r\n",
                   (unsigned long)record_offset,
                   (unsigned int)bytes_read,
                   (unsigned int)frame_length);
            break;
        }

        DeviceProtocol_ParserReset(&parser);

        /*解析从sd卡里面读出来的数据，并保存到frame结构体里面去*/
        for (index = 0U; index < frame_length; index++)
        {
            if (DeviceProtocol_ParserFeed(&parser, encoded_frame[index], &frame) != 0U)
            {
                break;
            }
        }

        if (index >= frame_length)
        {
            printf("STORAGE RECORD CRC/FORMAT ERROR: OFFSET=%lu LEN=%u\r\n",
                   (unsigned long)record_offset, (unsigned int)frame_length);
            break;
        }

        record_count++;
        printf("STORAGE RECORD OK: INDEX=%lu ID=%08lX SEQ=%lu TIME=%lu LEN=%u\r\n",
               (unsigned long)record_count,
               (unsigned long)frame.device_id,
               (unsigned long)frame.sequence,
               (unsigned long)frame.timestamp,
               (unsigned int)frame_length);
    }

    /* 校验结束后恢复到文件末尾，后续追加不会覆盖已有记录。 */
    result = f_lseek(file, file_size);
    if (result == FR_OK)
    {
        printf("STORAGE VERIFY END: VALID=%lu\r\n", (unsigned long)record_count);
    }
    return result;
}

void StorageService_Init(void)
{
    /*上传位置默认为0*/
    upload_offset_cache = 0U;
    /*无就绪数据*/
    upload_offset_ready = 0U;

    /*默认无上传进度副本*/
    upload_progress_generation = 0U;
    pending_history_next_offset = 0U;
    pending_history_offset_valid = 0U;

    /* 队列在任务启动前创建，消息内容由CMSIS-RTOS复制，不依赖调用者缓冲区寿命。 */
    storage_queue = osMessageQueueNew(STORAGE_QUEUE_DEPTH, sizeof(StorageCommand_t),NULL);
    /* 读取结果单独排队，StorageTask和NetworkTask之间不共享临时缓冲区。 */
    storage_read_queue = osMessageQueueNew(STORAGE_READ_QUEUE_DEPTH,
                                           sizeof(StorageReadResult_t), NULL);
    if ((storage_queue == NULL) || (storage_read_queue == NULL))
    {
        printf("STORAGE QUEUE CREATE FAILED: CMD=%p RESULT=%p\r\n",
               storage_queue, storage_read_queue);
        return;
    }

    /* 上电先检查卡中已有记录，仅验证和打印，不发送也不修改文件。 */
    (void)StorageService_RequestVerifyAll();
}

uint8_t StorageService_AppendFrame(const uint8_t *frame, uint16_t length)
{
    StorageCommand_t command; /* 复制完整帧后投递，避免依赖调用者缓冲区寿命。 */

    if ((storage_queue == NULL) || (frame == NULL) ||
        (length == 0U) || (length > DEVICE_PROTOCOL_MAX_FRAME_SIZE))
    {
        return 0U;
    }

    command.type = STORAGE_COMMAND_APPEND;
    command.length = length;
    memcpy(command.frame, frame, length);

    /* 网络任务不能等待SD卡；队列满时立即返回，由调用者打印丢记录提示。 */
    return (osMessageQueuePut(storage_queue, &command, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t StorageService_RequestVerifyAll(void)
{
    StorageCommand_t command; /* 不携带帧数据，只通知StorageTask执行全文件校验。 */

    if (storage_queue == NULL)
    {
        return 0U;
    }

    memset(&command, 0, sizeof(command));
    command.type = STORAGE_COMMAND_VERIFY_ALL;
    return (osMessageQueuePut(storage_queue, &command, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t StorageService_RequestReadNextPending(void)
{
    StorageCommand_t command; /* 无帧数据，只通知StorageTask读取当前待上传记录。 */

    if (storage_queue == NULL)
    {
        return 0U;
    }

    memset(&command, 0, sizeof(command));
    command.type = STORAGE_COMMAND_READ_NEXT_PENDING;
    return (osMessageQueuePut(storage_queue, &command, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t StorageService_TakeReadResult(StorageReadResult_t *result)
{
    if ((storage_read_queue == NULL) || (result == NULL))
    {
        return 0U;
    }

    /* 网络任务只取队列副本，不接触StorageTask内部缓冲区或FatFs文件对象。 */
    return (osMessageQueueGet(storage_read_queue, result, NULL, 0U) == osOK) ? 1U : 0U;
}

uint8_t StorageService_SetUploadOffset(uint32_t offset)
{
    StorageCommand_t command; /* 保存目标偏移，实际文件写入在StorageTask中完成。 */

    if (storage_queue == NULL)
    {
        return 0U;
    }

    memset(&command, 0, sizeof(command));
    command.type = STORAGE_COMMAND_SET_UPLOAD_OFFSET;
    command.offset = offset;
    return (osMessageQueuePut(storage_queue, &command, 0U, 0U) == osOK) ? 1U : 0U;
}

uint8_t StorageService_GetUploadOffset(uint32_t *offset)
{
    if ((offset == NULL) || (upload_offset_ready == 0U))
    {
        return 0U;
    }

    *offset = upload_offset_cache;
    return 1U;
}

void StorageService_Run(void)
{
    FIL file;                       /* records.bin文件对象，由StorageTask独占。 */
    FRESULT result;                 /* 当前文件系统操作结果。 */
    StorageCommand_t command;       /* 正在执行的队列命令，失败时保留用于重试。 */
    UINT written;                   /* 本次f_write实际写入的字节数。 */
    uint8_t mounted = 0U;           /* 1表示FatFs卷已经成功挂载。 */
    uint8_t file_open = 0U;         /* 1表示records.bin已经成功打开。 */
    uint8_t record_pending = 0U;    /* 1表示局部command尚未成功处理。 */
    uint8_t disk_record[2U + DEVICE_PROTOCOL_MAX_FRAME_SIZE]; /* 长度头与协议帧组合写缓存。 */
    FSIZE_t record_offset = 0U;     /* 追加前的文件位置，用于失败后清理半条记录。 */

    for (;;)
    {
        if (mounted == 0U)
        {
            /* 立即挂载会沿FatFs磁盘层完成HAL_SD_Init和4位总线配置。 */
            result = f_mount(&SDFatFS, SDPath, 1U);
            if (result != FR_OK)
            {
                printf("STORAGE MOUNT FAILED: %d\r\n", (int)result);
                osDelay(STORAGE_RETRY_DELAY_MS);
                continue;
            }

            mounted = 1U;
            printf("STORAGE MOUNT OK\r\n");
        }

        if (file_open == 0U)
        {
            /* 同一个文件对象既用于追加，也用于StorageTask内部的只读校验。 */
            result = f_open(&file, STORAGE_RECORD_FILE, FA_OPEN_ALWAYS | FA_READ | FA_WRITE);
            if (result == FR_OK)
            {
                /* FA_OPEN_ALWAYS默认位于文件开头，追加前必须移动到现有文件末尾。 */
                file_open = 1U;
                result = f_lseek(&file, f_size(&file));
            }

            if (result != FR_OK)
            {
                printf("STORAGE OPEN FAILED: %d\r\n", (int)result);
                /* f_open成功但f_lseek失败时，先释放文件对象再卸载卷。 */
                if (file_open != 0U)
                {
                    (void)f_close(&file);
                    file_open = 0U;
                }
                (void)f_mount(NULL, SDPath, 0U);
                mounted = 0U;
                osDelay(STORAGE_RETRY_DELAY_MS);
                continue;
            }

            printf("STORAGE FILE READY: %s\r\n", STORAGE_RECORD_FILE);

            if (upload_offset_ready == 0U)
            {
                result = StorageService_LoadUploadProgress(&file);
                if (result != FR_OK)
                {
                    printf("UPLOAD PROGRESS LOAD FAILED: %d\r\n", (int)result);
                    (void)f_close(&file);
                    (void)f_mount(NULL, SDPath, 0U);
                    file_open = 0U;
                    mounted = 0U;
                    osDelay(STORAGE_RETRY_DELAY_MS);
                    continue;
                }
            }
        }

        if (record_pending == 0U)
        {
            /* 队列没有数据时任务休眠，不轮询SD卡，也不占用网络任务运行时间。 */
            if (osMessageQueueGet(storage_queue, &command, NULL, osWaitForever) != osOK)
            {
                continue;
            }
            record_pending = 1U;
        }

        if (command.type == STORAGE_COMMAND_VERIFY_ALL)
        {
            result = StorageService_VerifyAllRecords(&file);
            if (result == FR_OK)
            {
                record_pending = 0U;
                continue;
            }

            printf("STORAGE VERIFY READ FAILED: %d\r\n", (int)result);
        }

        if (command.type == STORAGE_COMMAND_SET_UPLOAD_OFFSET)
        {
            result = StorageService_SaveUploadProgress(&file, command.offset);
            if (result == FR_OK)
            {
                record_pending = 0U;
                continue;
            }

            if (result == FR_INVALID_PARAMETER)
            {
                /* 非记录边界属于调用参数错误，丢弃请求，不能反复卸载并重试。 */
                record_pending = 0U;
                continue;
            }

            printf("UPLOAD PROGRESS SAVE FAILED: %d\r\n", (int)result);
        }

        if (command.type == STORAGE_COMMAND_READ_NEXT_PENDING)
        {
            result = StorageService_ReadNextPendingRecord(&file);
            if (result == FR_OK)
            {
                record_pending = 0U;
                continue;
            }

            printf("STORAGE PENDING READ FAILED: %d\r\n", (int)result);
        }

        if ((result == FR_OK) && (command.type == STORAGE_COMMAND_APPEND))
        {
            uint16_t disk_length = (uint16_t)(command.length + 2U);

            record_offset = f_tell(&file);
            disk_record[0] = (uint8_t)(command.length >> 8U);
            disk_record[1] = (uint8_t)command.length;
            memcpy(&disk_record[2], command.frame, command.length);

            /* 长度头和协议帧一次写入，避免两个f_write之间掉电留下孤立长度头。 */
            result = f_write(&file, disk_record, disk_length, &written);
            if ((result == FR_OK) && (written != disk_length))
            {
                result = FR_DISK_ERR;
            }

            /* 每条记录同步一次，拔卡或掉电时最多影响正在写入的这一条记录。 */
            if (result == FR_OK)
            {
                result = f_sync(&file);
            }
        }

        if (result == FR_OK)
        {
            printf("STORAGE APPEND OK: SEQ=%lu LEN=%u\r\n",
                   (unsigned long)(((uint32_t)command.frame[8] << 24U) |
                                   ((uint32_t)command.frame[9] << 16U) |
                                   ((uint32_t)command.frame[10] << 8U) |
                                   command.frame[11]),
                   (unsigned int)command.length);
            record_pending = 0U;
        }
        else
        {
            /* 当前记录保留在任务局部变量中，重新挂载后继续尝试，不从队列取下一条。 */
            printf("STORAGE IO FAILED: %d, RETRY PENDING COMMAND\r\n", (int)result);
            /* 只有追加失败才清理可能写入的半条记录；只读校验失败绝不修改文件。 */
            if ((command.type == STORAGE_COMMAND_APPEND) &&
                (f_lseek(&file, record_offset) == FR_OK))
            {
                (void)f_truncate(&file);
                (void)f_sync(&file);
            }
            (void)f_close(&file);
            (void)f_mount(NULL, SDPath, 0U);
            file_open = 0U;
            mounted = 0U;
            osDelay(STORAGE_RETRY_DELAY_MS);
        }
    }
}
