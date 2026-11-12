//
//  qca_compat.h
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Clean kernel compatibility definitions for Darwin / IOKit.
//  Guaranteed zero-conflict with macOS SDK / libkern.
//

#ifndef _QCA_COMPAT_H_
#define _QCA_COMPAT_H_

#include <libkern/OSTypes.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <IOKit/IOLib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Integer types compatible with Linux/Atheros definitions */
#ifndef _LINUX_TYPES_H
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint16_t  __le16;
typedef uint32_t  __le32;
typedef uint64_t  __le64;
typedef uint16_t  __be16;
typedef uint32_t  __be32;
typedef uint64_t  __be64;

typedef uint64_t  dma_addr_t;
#endif

/* Endianness conversion helpers */
#define le16_to_cpu(x) OSSwapLittleToHostInt16((uint16_t)(x))
#define cpu_to_le16(x) OSSwapHostToLittleInt16((uint16_t)(x))
#define le32_to_cpu(x) OSSwapLittleToHostInt32((uint32_t)(x))
#define cpu_to_le32(x) OSSwapHostToLittleInt32((uint32_t)(x))
#define le64_to_cpu(x) OSSwapLittleToHostInt64((uint64_t)(x))
#define cpu_to_le64(x) OSSwapHostToLittleInt64((uint64_t)(x))

#define be16_to_cpu(x) OSSwapBigToHostInt16((uint16_t)(x))
#define cpu_to_be16(x) OSSwapHostToBigInt16((uint16_t)(x))
#define be32_to_cpu(x) OSSwapBigToHostInt32((uint32_t)(x))
#define cpu_to_be32(x) OSSwapHostToBigInt32((uint32_t)(x))

/* Bit manipulation macros */
#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif

#ifndef BITS_PER_LONG
#define BITS_PER_LONG 64
#endif

#ifndef GENMASK
#define GENMASK(h, l) \
    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (BITS_PER_LONG - 1 - (h))))
#endif

#ifndef FIELD_GET
#define FIELD_GET(_mask, _reg) (((_reg) & (_mask)) >> (__builtin_ctzll(_mask)))
#endif

#ifndef FIELD_PREP
#define FIELD_PREP(_mask, _val) (((_val) << (__builtin_ctzll(_mask))) & (_mask))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif

#ifndef max
#define max(x, y) ((x) > (y) ? (x) : (y))
#endif

#ifndef clamp
#define clamp(val, lo, hi) (min(max(val, lo), hi))
#endif

#ifndef round_up
#define round_up(x, y) ((((x)-1) | __typeof__((y)-1)(y)-1) + 1)
#endif

/* Kernel logging helpers */
#define QCA_LOG_PREFIX "AirportQCA: "

#define qca_info(fmt, ...) \
    IOLog(QCA_LOG_PREFIX "[INFO] " fmt "\n", ##__VA_ARGS__)

#define qca_warn(fmt, ...) \
    IOLog(QCA_LOG_PREFIX "[WARN] " fmt "\n", ##__VA_ARGS__)

#define qca_err(fmt, ...) \
    IOLog(QCA_LOG_PREFIX "[ERR] " fmt "\n", ##__VA_ARGS__)

#ifdef DEBUG
#define qca_dbg(fmt, ...) \
    IOLog(QCA_LOG_PREFIX "[DBG] " fmt "\n", ##__VA_ARGS__)
#else
#define qca_dbg(fmt, ...) do {} while (0)
#endif

/* Delay helpers */
static inline void qca_udelay(unsigned int usecs) {
    IODelay(usecs);
}

static inline void qca_mdelay(unsigned int msecs) {
    for (unsigned int i = 0; i < msecs; i++) {
        IODelay(1000);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* _QCA_COMPAT_H_ */
