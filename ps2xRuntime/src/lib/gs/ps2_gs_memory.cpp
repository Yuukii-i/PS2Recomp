#include <array>
#include <atomic>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <intrin.h>

#include "runtime/gs/ps2_gs_memory.h"

void ps2xSafeLog(const char *fmt, ...); // PERF-LO (global, defined elsewhere)

namespace GSMem
{
    using C32Traits  = PixelStorageTraits<C32>;
    using Z32Traits  = PixelStorageTraits<Z32>;
    using C16Traits  = PixelStorageTraits<C16>;
    using C16STraits = PixelStorageTraits<C16S>;
    using Z16Traits  = PixelStorageTraits<Z16>;
    using Z16STraits = PixelStorageTraits<Z16S>;
    using P8Traits   = PixelStorageTraits<P8>;
    using P4Traits   = PixelStorageTraits<P4>;

    using C32PageLookupTable    = C32Traits::PageLookupTableT;
    using C32BlockLookupTableT  = C32Traits::BlockLookupTableT;
    using Z32PageLookupTableT = Z32Traits::PageLookupTableT;
    using Z32BlockLookupTableT = Z32Traits::BlockLookupTableT;

    // column table is shared
    using C32ColumnLookupTableT = C32Traits::ColumnLookupTableT;

    using C16PageLookupTable   = C16Traits::PageLookupTableT;
    using C16BlockLookupTable  = C16Traits::BlockLookupTableT;
    using Z16PageLookupTable   = Z16Traits::PageLookupTableT;
    using Z16BlockLookupTable  = Z16Traits::BlockLookupTableT;

    using C16SPageLookupTable  = C16STraits::PageLookupTableT;
    using C16SBlockLookupTable = C16STraits::BlockLookupTableT;
    using Z16SPageLookupTable  = Z16STraits::PageLookupTableT;
    using Z16SBlockLookupTable = Z16STraits::BlockLookupTableT;

    // column table is shared
    using C16ColumnLookupTable = C16Traits::ColumnLookupTableT;

    using P8PageLookupTable   = P8Traits::PageLookupTableT;
    using P8BlockLookupTable  = P8Traits::BlockLookupTableT;
    using P8ColumnLookupTable = P8Traits::ColumnLookupTableT;

    using P4PageLookupTable   = P4Traits::PageLookupTableT;
    using P4BlockLookupTable  = P4Traits::BlockLookupTableT;
    using P4ColumnLookupTable = P4Traits::ColumnLookupTableT;

    static constexpr C32BlockLookupTableT BlockTableC32
    {{
        {  0,  1,  4,  5, 16, 17, 20, 21 },
        {  2,  3,  6,  7, 18, 19, 22, 23 },
        {  8,  9, 12, 13, 24, 25, 28, 29 },
        { 10, 11, 14, 15, 26, 27, 30, 31 }
    }};

    static constexpr Z32BlockLookupTableT BlockTableZ32
    {{
        { 24, 25, 28, 29,  8,  9, 12, 13 },
        { 26, 27, 30, 31, 10, 11, 14, 15 },
        { 16, 17, 20, 21,  0,  1,  4,  5 },
        { 18, 19, 22, 23,  2,  3,  6,  7 }
    }};

    static constexpr C16BlockLookupTable BlockTableC16
    {{
        {  0,  2,  8, 10 },
        {  1,  3,  9, 11 },
        {  4,  6, 12, 14 },
        {  5,  7, 13, 15 },
        { 16, 18, 24, 26 },
        { 17, 19, 25, 27 },
        { 20, 22, 28, 30 },
        { 21, 23, 29, 31 }
    }};

    static constexpr C16SBlockLookupTable BlockTableC16S
    {{
        {  0,  2, 16, 18 },
        {  1,  3, 17, 19 },
        {  8, 10, 24, 26 },
        {  9, 11, 25, 27 },
        {  4,  6, 20, 22 },
        {  5,  7, 21, 23 },
        { 12, 14, 28, 30 },
        { 13, 15, 29, 31 }
    }};

    static constexpr Z16BlockLookupTable BlockTableZ16
    {{
        { 24, 26, 16, 18 },
        { 25, 27, 17, 19 },
        { 28, 30, 20, 22 },
        { 29, 31, 21, 23 },
        {  8, 10,  0,  2 },
        {  9, 11,  1,  3 },
        { 12, 14,  4,  6 },
        { 13, 15,  5,  7 }
    }};

    static constexpr Z16SBlockLookupTable BlockTableZ16S
    {{
        { 24, 26,  8, 10 },
        { 25, 27,  9, 11 },
        { 16, 18,  0,  2 },
        { 17, 19,  1,  3 },
        { 28, 30, 12, 14 },
        { 29, 31, 13, 15 },
        { 20, 22,  4,  6 },
        { 21, 23,  5,  7 }
    }};

    static constexpr P8BlockLookupTable BlockTableP8
    {{
        {  0,  1,  4,  5, 16, 17, 20, 21},
        {  2,  3,  6,  7, 18, 19, 22, 23},
        {  8,  9, 12, 13, 24, 25, 28, 29},
        { 10, 11, 14, 15, 26, 27, 30, 31}
    }};

    static constexpr P4BlockLookupTable BlockTableP4
    {{
        {  0,  2,  8, 10 },
        {  1,  3,  9, 11 },
        {  4,  6, 12, 14 },
        {  5,  7, 13, 15 },
        { 16, 18, 24, 26 },
        { 17, 19, 25, 27 },
        { 20, 22, 28, 30 },
        { 21, 23, 29, 31 }
    }};

    // column layout is shared between c32 and z32
    static constexpr C32ColumnLookupTableT ColumnTable32
    {{
        {  0,  1,  4,  5,  8,  9, 12, 13 },
        {  2,  3,  6,  7, 10, 11, 14, 15 },
        { 16, 17, 20, 21, 24, 25, 28, 29 },
        { 18, 19, 22, 23, 26, 27, 30, 31 },
        { 32, 33, 36, 37, 40, 41, 44, 45 },
        { 34, 35, 38, 39, 42, 43, 46, 47 },
        { 48, 49, 52, 53, 56, 57, 60, 61 },
        { 50, 51, 54, 55, 58, 59, 62, 63 },
    }};

    static constexpr C16ColumnLookupTable ColumnTable16
    {{
        {   0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27 },
        {   4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31 },
        {  32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59 },
        {  36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63 },
        {  64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91 },
        {  68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95 },
        {  96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123 },
        { 100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127 }
    }};

    static constexpr P8ColumnLookupTable ColumnTable8
    {{
        {   0,   4,  16,  20,  32,  36,  48,  52,   2,   6,  18,  22,  34,  38,  50,  54 },
        {   8,  12,  24,  28,  40,  44,  56,  60,  10,  14,  26,  30,  42,  46,  58,  62 },
        {  33,  37,  49,  53,   1,   5,  17,  21,  35,  39,  51,  55,   3,   7,  19,  23 },
        {  41,  45,  57,  61,   9,  13,  25,  29,  43,  47,  59,  63,  11,  15,  27,  31 },
        {  96, 100, 112, 116,  64,  68,  80,  84,  98, 102, 114, 118,  66,  70,  82,  86 },
        { 104, 108, 120, 124,  72,  76,  88,  92, 106, 110, 122, 126,  74,  78,  90,  94 },
        {  65,  69,  81,  85,  97, 101, 113, 117,  67,  71,  83,  87,  99, 103, 115, 119 },
        {  73,  77,  89,  93, 105, 109, 121, 125,  75,  79,  91,  95, 107, 111, 123, 127 },
        { 128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182 },
        { 136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190 },
        { 161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151 },
        { 169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159 },
        { 224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214 },
        { 232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222 },
        { 193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247 },
        { 201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255 }
    }};

    static constexpr P4ColumnLookupTable ColumnTable4
    {{
        {   0,   8,  32,  40,  64,  72,  96, 104,   2,  10,  34,  42,  66,  74,  98, 106,   4,  12,  36,  44,  68,  76, 100, 108,   6,  14,  38,  46,  70,  78, 102, 110 },
        {  16,  24,  48,  56,  80,  88, 112, 120,  18,  26,  50,  58,  82,  90, 114, 122,  20,  28,  52,  60,  84,  92, 116, 124,  22,  30,  54,  62,  86,  94, 118, 126 },
        {  65,  73,  97, 105,   1,   9,  33,  41,  67,  75,  99, 107,   3,  11,  35,  43,  69,  77, 101, 109,   5,  13,  37,  45,  71,  79, 103, 111,   7,  15,  39,  47 },
        {  81,  89, 113, 121,  17,  25,  49,  57,  83,  91, 115, 123,  19,  27,  51,  59,  85,  93, 117, 125,  21,  29,  53,  61,  87,  95, 119, 127,  23,  31,  55,  63 },
        { 192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174 },
        { 208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190 },
        { 129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239 },
        { 145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255 },
        { 256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366 },
        { 272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382 },
        { 321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303 },
        { 337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319 },
        { 448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430 },
        { 464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446 },
        { 385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495 },
        { 401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511 },
    }};

    // this is going to be massive (an entire page of addess lookups)
    static C32PageLookupTable  PageTableC32{ };
    static Z32PageLookupTableT PageTableZ32{ };
    static C16PageLookupTable  PageTableC16{ };
    static C16SPageLookupTable PageTableC16S{ };
    static Z16PageLookupTable  PageTableZ16{ };
    static Z16SPageLookupTable PageTableZ16S{ };
    static P8PageLookupTable   PageTableP8{ };
    static P4PageLookupTable   PageTableP4{ };

    // PERF-VK: export the page lookup tables so the Vulkan backend uses the exact same swizzle. idx: 0 C32, 1 Z32, 2 C16,
    // 3 C16S, 4 Z16, 5 Z16S, 6 P8, 7 P4. Entries are u16, layout [block][y][x].
    const uint16_t *ExportPageTable(int idx, size_t *entries)
    {
        switch (idx)
        {
        case 0: *entries = sizeof(PageTableC32) / 2; return reinterpret_cast<const uint16_t *>(&PageTableC32);
        case 1: *entries = sizeof(PageTableZ32) / 2; return reinterpret_cast<const uint16_t *>(&PageTableZ32);
        case 2: *entries = sizeof(PageTableC16) / 2; return reinterpret_cast<const uint16_t *>(&PageTableC16);
        case 3: *entries = sizeof(PageTableC16S) / 2; return reinterpret_cast<const uint16_t *>(&PageTableC16S);
        case 4: *entries = sizeof(PageTableZ16) / 2; return reinterpret_cast<const uint16_t *>(&PageTableZ16);
        case 5: *entries = sizeof(PageTableZ16S) / 2; return reinterpret_cast<const uint16_t *>(&PageTableZ16S);
        case 6: *entries = sizeof(PageTableP8) / 2; return reinterpret_cast<const uint16_t *>(&PageTableP8);
        case 7: *entries = sizeof(PageTableP4) / 2; return reinterpret_cast<const uint16_t *>(&PageTableP4);
        }
        *entries = 0;
        return nullptr;
    }

    void InitLookupTables()
    {
        // 32 bit
        PixelStorageTraits<C32>::InitPageLookupTable(PageTableC32, BlockTableC32, ColumnTable32);
        PixelStorageTraits<Z32>::InitPageLookupTable(PageTableZ32, BlockTableZ32, ColumnTable32);

        // 16 bit
        PixelStorageTraits<C16>::InitPageLookupTable(PageTableC16, BlockTableC16, ColumnTable16);
        PixelStorageTraits<C16S>::InitPageLookupTable(PageTableC16S, BlockTableC16S, ColumnTable16);
        PixelStorageTraits<Z16>::InitPageLookupTable(PageTableZ16, BlockTableZ16, ColumnTable16);
        PixelStorageTraits<Z16S>::InitPageLookupTable(PageTableZ16S, BlockTableZ16S, ColumnTable16);

        // 8 bit
        PixelStorageTraits<P8>::InitPageLookupTable(PageTableP8, BlockTableP8, ColumnTable8);

        // 4 bit
        PixelStorageTraits<P4>::InitPageLookupTable(PageTableP4, BlockTableP4, ColumnTable4);
    }

    u32 ReadTexture(TexturePageCache& cache, const u8* data, u32 psm, u32 bp, u32 bw, u32 x, u32 y)
    {
        switch (static_cast<PixelStorageMode>(psm & 0x3Fu))
        {
        case C32:
            return PixelStorageTraits<C32>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case C24:
            return PixelStorageTraits<C24>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case C16:
            return PixelStorageTraits<C16>::Read(PageTableC16, data, bp, bw, x, y, &cache);
        case C16S:
            return PixelStorageTraits<C16S>::Read(PageTableC16S, data, bp, bw, x, y, &cache);
        case P8:
            return PixelStorageTraits<P8>::Read(PageTableP8, data, bp, bw, x, y, &cache);
        case P4:
            return PixelStorageTraits<P4>::Read(PageTableP4, data, bp, bw, x, y, &cache);
        case P8H:
            return PixelStorageTraits<P8H>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case P4HL:
            return PixelStorageTraits<P4HL>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case P4HH:
            return PixelStorageTraits<P4HH>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case Z32:
            return PixelStorageTraits<Z32>::Read(PageTableZ32, data, bp, bw, x, y, &cache);
        case Z24:
            return PixelStorageTraits<Z24>::Read(PageTableZ32, data, bp, bw, x, y, &cache);
        case Z16:
            return PixelStorageTraits<Z16>::Read(PageTableZ16, data, bp, bw, x, y, &cache);
        case Z16S:
            return PixelStorageTraits<Z16S>::Read(PageTableZ16S, data, bp, bw, x, y, &cache);
        default:
            return 0u;
        }
    }

    void WriteCT32(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C32>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteCT24(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C24>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteZ32(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z32>::Write(PageTableZ32, data, bp, bw, x, y, value);
    }

    void WriteZ24(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z24>::Write(PageTableZ32, data, bp, bw, x, y, value);
    }

    void WriteCT16(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C16>::Write(PageTableC16, data, bp, bw, x, y, value);
    }

    void WriteCT16S(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C16S>::Write(PageTableC16S, data, bp, bw, x, y, value);
    }

    void WriteZ16(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z16>::Write(PageTableZ16, data, bp, bw, x, y, value);
    }

    void WriteZ16S(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z16S>::Write(PageTableZ16S, data, bp, bw, x, y, value);
    }

    void WriteP8(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P8>::Write(PageTableP8, data, bp, bw, x, y, value);
    }

    void WriteP8H(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P8H>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteP4(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4>::Write(PageTableP4, data, bp, bw, x, y, value);
    }

    void WriteP4HL(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4HL>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteP4HH(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4HH>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteNull(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
    }

    u32 ReadCT32(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C32>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadZ32(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z32>::Read(PageTableZ32, data, bp, bw, x, y);
    }

    u32 ReadCT24(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C24>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadZ24(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z24>::Read(PageTableZ32, data, bp, bw, x, y);
    }

    u32 ReadCT16(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C16>::Read(PageTableC16, data, bp, bw, x, y);
    }

    u32 ReadCT16S(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C16S>::Read(PageTableC16S, data, bp, bw, x, y);
    }

    u32 ReadZ16(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z16>::Read(PageTableZ16, data, bp, bw, x, y);
    }

    u32 ReadZ16S(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z16S>::Read(PageTableZ16S, data, bp, bw, x, y);
    }

    u32 ReadP8(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P8>::Read(PageTableP8, data, bp, bw, x, y);
    }

    u32 ReadP8H(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P8H>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadP4(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4>::Read(PageTableP4, data, bp, bw, x, y);
    }

    u32 ReadP4HL(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4HL>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadP4HH(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4HH>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadNull(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return 0;
    }

    // PERF-GU: bulk host->local upload. Uses the very same PixelStorageTraits<>::Write as the per-pixel wrappers above; only the per-pixel
    // indirect call, the format switch and the two divisions of the transfer bookkeeping are gone (x/y advance incrementally).
    // kind: 0 CT32, 1 CT24, 2 CT16, 3 CT16S, 4 T8, 5 T8H, 6 T4, 7 T4HL, 8 T4HH, 9 Z32, 10 Z24, 11 Z16, 12 Z16S
    namespace
    {
        template <PixelStorageMode PSM, int BYTES, typename TableT>
        void StreamBytes(const TableT &table, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            using Traits = PixelStorageTraits<PSM>;
            for (u32 i = 0; i < count; ++i)
            {
                u32 v;
                if constexpr (BYTES == 4)
                    std::memcpy(&v, src + i * 4u, 4);
                else if constexpr (BYTES == 3)
                    v = static_cast<u32>(src[i * 3u]) | (static_cast<u32>(src[i * 3u + 1u]) << 8u) | (static_cast<u32>(src[i * 3u + 2u]) << 16u);
                else if constexpr (BYTES == 2)
                {
                    uint16_t h;
                    std::memcpy(&h, src + i * 2u, 2);
                    v = h;
                }
                else
                    v = src[i];
                Traits::Write(table, data, bp, bw, x, y, v);
                if (++x == xEnd)
                {
                    x = xBegin;
                    ++y;
                }
            }
        }

        // PERF-GU2: same address as PixelStorageTraits<>::Address / Write for the non-read-modify-write formats, with everything that only depends on
        // the row (page row, lookup-table row) hoisted out of the pixel loop. Verified against the per-pixel wrappers (tests in the scratchpad harness).
        template <PixelStorageMode PSM, int BYTES, typename TableT>
        void StreamBytesFast(const TableT &table, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            using Traits = PixelStorageTraits<PSM>;
            constexpr u32 PW = Traits::PageExtent().x;
            constexpr u32 PH = Traits::PageExtent().y;
            constexpr usz PPP = Traits::PixelsPerPage();
            constexpr usz BPP = Traits::BlocksPerPage();
            const u32 blockIdx = static_cast<u32>(bp % BPP);
            const u32 basePage = static_cast<u32>(bp / BPP);
            const u32 pagesPerRow = (bw * 64u) / PW;
            u32 done = 0;
            while (done < count)
            {
                const u32 n = std::min<u32>(count - done, xEnd - x);
                const auto &trow = table[blockIdx][y % PH];
                const usz rowPage = static_cast<usz>(basePage) + static_cast<usz>((y / PH) * pagesPerRow);
                for (u32 k = 0; k < n; ++k)
                {
                    const u32 xx = x + k;
                    const usz page = rowPage + xx / PW;
                    const u32 pixelAddr = static_cast<u32>(page * PPP + trow[xx % PW]);
                    const u32 bits = static_cast<u32>(static_cast<usz>(pixelAddr) * UnpackedBitWidth(PSM) + Traits::BitOffset());
                    const u32 byteAddr = (bits / 8u) & static_cast<u32>(MEMORY_SIZE - BYTES);
                    std::memcpy(data + byteAddr, src + (done + k) * BYTES, BYTES);
                }
                done += n;
                x += n;
                if (x == xEnd)
                {
                    x = xBegin;
                    ++y;
                }
            }
        }

        template <PixelStorageMode PSM, typename TableT>
        void StreamNibbles(const TableT &table, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            using Traits = PixelStorageTraits<PSM>;
            for (u32 i = 0; i < count; ++i)
            {
                const u32 v = (src[i >> 1] >> ((i & 1u) * 4u)) & 0x0Fu;
                Traits::Write(table, data, bp, bw, x, y, v);
                if (++x == xEnd)
                {
                    x = xBegin;
                    ++y;
                }
            }
        }
    }

    static bool UploadStreamOld(int kind, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
    {
        switch (kind)
        {
        case 0:  StreamBytesFast<C32, 4>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 1:  StreamBytes<C24, 3>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 2:  StreamBytesFast<C16, 2>(PageTableC16, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 3:  StreamBytesFast<C16S, 2>(PageTableC16S, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 4:  StreamBytesFast<P8, 1>(PageTableP8, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 5:  StreamBytes<P8H, 1>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 6:  StreamNibbles<P4>(PageTableP4, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 7:  StreamNibbles<P4HL>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 8:  StreamNibbles<P4HH>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 9:  StreamBytesFast<Z32, 4>(PageTableZ32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 10: StreamBytes<Z24, 3>(PageTableZ32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 11: StreamBytesFast<Z16, 2>(PageTableZ16, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 12: StreamBytesFast<Z16S, 2>(PageTableZ16S, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        default: return false;
        }
    }

    // PERF-LO (PS2X_UPLOAD_FAST=1): row-hoisted variants for the kinds that still used the per-pixel Traits::Write (1 CT24, 5 T8H, 10 Z24, 6/7/8 T4*).
    // Same arithmetic as PixelStorageTraits<>::Address/Write, with the page row and table row hoisted out of the pixel loop.
    // MODE 0: 3 source bytes, read-modify-write of a u32 (keeps the top byte); 1: one source byte stored as-is (T8H); 2: 4-bit pixel, RMW of a byte.
    namespace
    {
        template <PixelStorageMode PSM, int MODE, typename TableT>
        void StreamFast2(const TableT &table, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            using Traits = PixelStorageTraits<PSM>;
            constexpr u32 PW = Traits::PageExtent().x;
            constexpr u32 PH = Traits::PageExtent().y;
            constexpr usz PPP = Traits::PixelsPerPage();
            constexpr usz BPP = Traits::BlocksPerPage();
            constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE - (MODE == 0 ? 4u : 1u));
            const u32 blockIdx = static_cast<u32>(bp % BPP);
            const u32 basePage = static_cast<u32>(bp / BPP);
            const u32 pagesPerRow = (bw * 64u) / PW;
            u32 done = 0;
            while (done < count)
            {
                const u32 n = std::min<u32>(count - done, xEnd - x);
                const auto &trow = table[blockIdx][y % PH];
                const usz rowPage = static_cast<usz>(basePage) + static_cast<usz>((y / PH) * pagesPerRow);
                for (u32 k = 0; k < n; ++k)
                {
                    const u32 xx = x + k;
                    const usz page = rowPage + xx / PW;
                    const u32 pixelAddr = static_cast<u32>(page * PPP + trow[xx % PW]);
                    const u32 bits = static_cast<u32>(static_cast<usz>(pixelAddr) * UnpackedBitWidth(PSM) + Traits::BitOffset());
                    const u32 byteAddr = (bits / 8u) & kMask;
                    u8 *ptr = data + byteAddr;
                    const u32 i = done + k;
                    if constexpr (MODE == 0)
                    {
                        const u32 v = static_cast<u32>(src[i * 3u]) | (static_cast<u32>(src[i * 3u + 1u]) << 8u) | (static_cast<u32>(src[i * 3u + 2u]) << 16u);
                        u32 old;
                        std::memcpy(&old, ptr, 4);
                        const u32 nv = (old & 0xFF000000u) | (v & 0x00FFFFFFu);
                        std::memcpy(ptr, &nv, 4);
                    }
                    else if constexpr (MODE == 1)
                    {
                        *ptr = src[i];
                    }
                    else
                    {
                        const u32 v = (src[i >> 1] >> ((i & 1u) * 4u)) & 0x0Fu;
                        const u32 shift = bits % 8u;
                        const u32 old = *ptr;
                        *ptr = static_cast<u8>((old & ~(0x0Fu << shift)) | ((v & 0x0Fu) << shift));
                    }
                }
                done += n;
                x += n;
                if (x == xEnd)
                {
                    x = xBegin;
                    ++y;
                }
            }
        }
    }

    // PERF-LP: kind 4 (PSMT8) whole-block path. The in-block byte layout is taken from the existing PageTableP8 and CHECKED once against every block of
    // every table row (a block is 16x16, its 256 pixels occupy one aligned 256-pixel span: table value = blockOrigin + inBlock[by*16+bx], with inBlock the same
    // everywhere). If the check fails the fast path is disabled and the old code runs. Aligned 16-row groups only; everything else uses StreamBytesFast.
    static std::atomic<uint64_t> g_t8FastCalls{0};
    static std::atomic<uint64_t> g_t8FastGroups{0};

    namespace
    {
        struct T8BlockTable
        {
            bool ok = false;
            u8 inBlock[256]; // destination byte offset (inside the block's 256-byte span) of source pixel by*16+bx
            T8BlockTable()
            {
                using Traits = PixelStorageTraits<P8>;
                constexpr u32 PW = Traits::PageExtent().x;
                constexpr u32 PH = Traits::PageExtent().y;
                constexpr usz BPP = Traits::BlocksPerPage();
                static_assert(PW % 16u == 0u && PH % 16u == 0u, "T8 page must be a multiple of the 16x16 block");
                static_assert(UnpackedBitWidth(P8) == 8 && Traits::BitOffset() == 0, "T8 is byte addressed");
                const u32 base = PageTableP8[0][0][0];
                if ((base & 0xFFu) != 0u)
                    return;
                for (u32 by = 0; by < 16u; ++by)
                    for (u32 bx = 0; bx < 16u; ++bx)
                    {
                        const u32 off = static_cast<u32>(PageTableP8[0][by][bx]) - base;
                        if (off >= 256u)
                            return;
                        inBlock[by * 16u + bx] = static_cast<u8>(off);
                    }
                for (u32 bi = 0; bi < BPP; ++bi)
                    for (u32 yb = 0; yb < PH; yb += 16u)
                        for (u32 xb = 0; xb < PW; xb += 16u)
                        {
                            const u32 org = PageTableP8[bi][yb][xb];
                            if ((org & 0xFFu) != 0u)
                                return;
                            for (u32 by = 0; by < 16u; ++by)
                                for (u32 bx = 0; bx < 16u; ++bx)
                                    if (static_cast<u32>(PageTableP8[bi][yb + by][xb + bx]) != org + inBlock[by * 16u + bx])
                                        return;
                        }
                ok = true;
            }
        };

        // Writes full aligned 16-row groups; returns the number of pixels consumed (a multiple of 16 * width). Position x/y are advanced.
        // Preconditions (checked by the caller): width % 16 == 0, xBegin % 16 == 0, x == xBegin, y % 16 == 0.
        u32 StreamT8Groups(const T8BlockTable &tbl, u8 *data, u32 bp, u32 bw, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            using Traits = PixelStorageTraits<P8>;
            constexpr u32 PW = Traits::PageExtent().x;
            constexpr u32 PH = Traits::PageExtent().y;
            constexpr usz PPP = Traits::PixelsPerPage();
            constexpr usz BPP = Traits::BlocksPerPage();
            constexpr u32 kMask = static_cast<u32>(MEMORY_SIZE - 1u);
            const u32 width = xEnd - xBegin;
            const u32 groupPixels = 16u * width;
            const u32 blockIdx = static_cast<u32>(bp % BPP);
            const u32 basePage = static_cast<u32>(bp / BPP);
            const u32 pagesPerRow = (bw * 64u) / PW;
            u32 consumed = 0;
            while (count - consumed >= groupPixels)
            {
                const auto &trow = PageTableP8[blockIdx][y % PH];
                const usz rowPage = static_cast<usz>(basePage) + static_cast<usz>((y / PH) * pagesPerRow);
                for (u32 xb = xBegin; xb < xEnd; xb += 16u)
                {
                    const usz page = rowPage + xb / PW;
                    const u32 pixelAddr = static_cast<u32>(page * PPP + trow[xb % PW]);
                    u8 *dst = data + (pixelAddr & kMask); // pixelAddr is 256-aligned, the block never wraps
                    const u8 *s = src + consumed + (xb - xBegin);
                    for (u32 by = 0; by < 16u; ++by, s += width)
                        for (u32 bx = 0; bx < 16u; ++bx)
                            dst[tbl.inBlock[by * 16u + bx]] = s[bx];
                }
                consumed += groupPixels;
                y += 16u;
            }
            return consumed;
        }

        void StreamT8(u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
        {
            static const T8BlockTable s_tbl; // thread-safe init
            const u32 width = xEnd - xBegin;
            if (!s_tbl.ok || width == 0u || (width & 15u) != 0u || (xBegin & 15u) != 0u)
            {
                StreamBytesFast<P8, 1>(PageTableP8, data, bp, bw, x, y, xBegin, xEnd, count, src);
                return;
            }
            // 1. leading part until the position is (xBegin, y % 16 == 0)
            while (count > 0 && !(x == xBegin && (y & 15u) == 0u))
            {
                const u32 n = (x != xBegin) ? std::min<u32>(count, xEnd - x) : std::min<u32>(count, width * (16u - (y & 15u)));
                StreamBytesFast<P8, 1>(PageTableP8, data, bp, bw, x, y, xBegin, xEnd, n, src);
                src += n;
                count -= n;
            }
            // 2. whole 16-row groups
            if (count >= 16u * width)
            {
                const u32 used = StreamT8Groups(s_tbl, data, bp, bw, y, xBegin, xEnd, count, src);
                if (used != 0u)
                {
                    g_t8FastGroups.fetch_add(used / (16u * width), std::memory_order_relaxed);
                    g_t8FastCalls.fetch_add(1, std::memory_order_relaxed);
                }
                src += used;
                count -= used;
            }
            // 3. tail
            if (count > 0)
                StreamBytesFast<P8, 1>(PageTableP8, data, bp, bw, x, y, xBegin, xEnd, count, src);
        }
    }

    static bool UploadStreamFast(int kind, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
    {
        switch (kind)
        {
        case 4:  StreamT8(data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 1:  StreamFast2<C24, 0>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 5:  StreamFast2<P8H, 1>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 6:  StreamFast2<P4, 2>(PageTableP4, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 7:  StreamFast2<P4HL, 2>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 8:  StreamFast2<P4HH, 2>(PageTableC32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        case 10: StreamFast2<Z24, 0>(PageTableZ32, data, bp, bw, x, y, xBegin, xEnd, count, src); return true;
        default: return UploadStreamOld(kind, data, bp, bw, x, y, xBegin, xEnd, count, src); // other kinds already use the hoisted StreamBytesFast
        }
    }

    // PERF-LO: UploadStream wrapper: PS2X_UPLOAD_STAT (counters), PS2X_UPLOAD_FAST (new variants), PS2X_UPLOAD_VERIFY (compare new vs old). All default off.
    static bool uploadEnvOn(const char *name)
    {
        const char *e = std::getenv(name);
        return e && e[0] == '1';
    }

    bool UploadStream(int kind, u8 *data, u32 bp, u32 bw, u32 &x, u32 &y, u32 xBegin, u32 xEnd, u32 count, const u8 *src)
    {
        static const bool s_stat = uploadEnvOn("PS2X_UPLOAD_STAT");
        static const bool s_fast = []() { const char *e = std::getenv("PS2X_UPLOAD_FAST"); const bool on = !(e && e[0] == '0'); /* PERF-LS: default ON */ if (on) { ::ps2xSafeLog("[uploadfast] ON\n"); std::fflush(stderr); } return on; }();
        static const bool s_verify = s_fast && uploadEnvOn("PS2X_UPLOAD_VERIFY");

        uint64_t t0 = 0;
        if (s_stat)
            t0 = __rdtsc();

        bool ok;
        if (!s_fast)
        {
            ok = UploadStreamOld(kind, data, bp, bw, x, y, xBegin, xEnd, count, src);
        }
        else if (!s_verify || ([]() {
                     static const uint64_t s_every = []() { const char *e = std::getenv("PS2X_UPLOAD_VERIFY_EVERY"); const unsigned long long n = e ? std::strtoull(e, nullptr, 10) : 64ull; return n ? static_cast<uint64_t>(n) : 1ull; }();
                     static uint64_t s_ctr = 0;
                     return (++s_ctr % s_every) != 0u;
                 }()))
        {
            ok = UploadStreamFast(kind, data, bp, bw, x, y, xBegin, xEnd, count, src);
        }
        else
        {
            // Verify: snapshot only the pages the call can touch (over-covering), run OLD, keep its result, restore, run NEW, compare.
            // The OLD result is always the one left in VRAM.
            static std::vector<u8> s_snap, s_oldRes;
            static std::vector<u32> s_spans; // byte offsets (8 KB aligned) of the covered pages
            static uint64_t s_verified = 0, s_mismatch = 0, s_fallback4mb = 0;
            constexpr u32 kPageBytes = 8192u;
            constexpr u32 kMemMask = static_cast<u32>(MEMORY_SIZE - 1u);
            s_spans.clear();
            bool whole = false;
            {
                u32 PW = 64, PH = 32, BPP = 32;
                switch (kind)
                {
                case 2: case 3: case 11: case 12: PW = 64; PH = 64; break;
                case 4: PW = 128; PH = 64; break;
                case 6: PW = 128; PH = 128; break;
                default: PW = 64; PH = 32; break;
                }
                // geometry cross-check against the traits for the formats we special-case, else fall back to the whole VRAM
                if ((kind == 4 && (PixelStorageTraits<P8>::PageExtent().x != PW || PixelStorageTraits<P8>::PageExtent().y != PH)) ||
                    (kind == 6 && (PixelStorageTraits<P4>::PageExtent().x != PW || PixelStorageTraits<P4>::PageExtent().y != PH)) ||
                    (kind == 0 && (PixelStorageTraits<C32>::PageExtent().x != PW || PixelStorageTraits<C32>::PageExtent().y != PH)) ||
                    (kind == 2 && (PixelStorageTraits<C16>::PageExtent().x != PW || PixelStorageTraits<C16>::PageExtent().y != PH)) ||
                    kind < 0 || kind > 12 || xEnd <= xBegin || bw == 0u || x < xBegin || x >= xEnd || bw > 1024u || count > (1u << 26))
                    whole = true;
                if (!whole)
                {
                    const u32 width = xEnd - xBegin;
                    const u32 rowsN = (x - xBegin + count + width - 1u) / width;
                    const u32 yLast = y + (rowsN ? rowsN - 1u : 0u);
                    const u32 pagesPerRow = (bw * 64u) / PW;
                    const u32 basePage = bp / BPP;
                    const u32 pxLo = xBegin / PW, pxHi = (xEnd - 1u) / PW;
                    const u64 nPages = static_cast<u64>(yLast / PH - y / PH + 1u) * (pxHi - pxLo + 3u);
                    if (nPages > 512u)
                        whole = true;
                    else
                    {
                        for (u32 r = y / PH; r <= yLast / PH; ++r)
                            for (u32 px = pxLo; px <= pxHi + 1u; ++px)
                            {
                                // one page of margin on each side (px - 1 handled by starting one lower)
                                const u32 pageIdx = basePage + r * pagesPerRow + px;
                                for (u32 m = 0; m < 2u; ++m)
                                    s_spans.push_back(((pageIdx - m) * kPageBytes) & kMemMask);
                            }
                    }
                }
            }
            if (!whole)
            {
                std::sort(s_spans.begin(), s_spans.end()); // each covered page copied once
                s_spans.erase(std::unique(s_spans.begin(), s_spans.end()), s_spans.end());
            }
            if (whole)
            {
                ++s_fallback4mb;
                s_spans.clear();
                for (u32 off = 0; off < MEMORY_SIZE; off += kPageBytes)
                    s_spans.push_back(off);
            }
            s_snap.resize(s_spans.size() * kPageBytes);
            s_oldRes.resize(s_spans.size() * kPageBytes);
            for (size_t i = 0; i < s_spans.size(); ++i)
                std::memcpy(s_snap.data() + i * kPageBytes, data + s_spans[i], kPageBytes);
            const u32 x0 = x, y0 = y;
            u32 xo = x, yo = y;
            ok = UploadStreamOld(kind, data, bp, bw, xo, yo, xBegin, xEnd, count, src);
            for (size_t i = 0; i < s_spans.size(); ++i)
                std::memcpy(s_oldRes.data() + i * kPageBytes, data + s_spans[i], kPageBytes);
            for (size_t i = 0; i < s_spans.size(); ++i)
                std::memcpy(data + s_spans[i], s_snap.data() + i * kPageBytes, kPageBytes);
            u32 xn = x, yn = y;
            UploadStreamFast(kind, data, bp, bw, xn, yn, xBegin, xEnd, count, src);
            bool same = (xn == xo && yn == yo);
            for (size_t i = 0; same && i < s_spans.size(); ++i)
                same = std::memcmp(data + s_spans[i], s_oldRes.data() + i * kPageBytes, kPageBytes) == 0;
            ++s_verified;
            if (!same)
            {
                ++s_mismatch;
                if (s_mismatch <= 20)
                {
                    ::ps2xSafeLog("[uploadfast] MISMATCH kind=%d x=%u y=%u w=%u h=%u off=%u\n", kind, x0, y0, xEnd - xBegin, count / ((xEnd - xBegin) ? (xEnd - xBegin) : 1u), x0 - xBegin);
                    std::fflush(stderr);
                }
            }
            // leave the OLD result in VRAM: restore each covered page from the saved OLD result (NOT a whole-VRAM copy: s_oldRes only holds the covered pages)
            for (size_t i = 0; i < s_spans.size(); ++i)
                std::memcpy(data + s_spans[i], s_oldRes.data() + i * kPageBytes, kPageBytes);
            {
                static uint32_t s_geomPrints = 0;
                if (s_geomPrints < 8u)
                {
                    ++s_geomPrints;
                    ::ps2xSafeLog("[uploadfast] verify geometry: kind=%d pages=%zu fallback=%d\n", kind, s_spans.size(), whole ? 1 : 0);
                    std::fflush(stderr);
                }
            }
            x = xo;
            y = yo;
            if ((s_verified & 255u) == 0u)
            {
                ::ps2xSafeLog("[uploadfast] verified=%llu mismatches=%llu fallback4MB=%llu\n", static_cast<unsigned long long>(s_verified), static_cast<unsigned long long>(s_mismatch), static_cast<unsigned long long>(s_fallback4mb));
                std::fflush(stderr);
            }
        }

        if (s_stat)
        {
            static uint64_t s_calls[13]{}, s_bytes[13]{}, s_ticks[13]{};
            static uint64_t s_winTsc = __rdtsc();
            static auto s_winT = std::chrono::steady_clock::now();
            if (kind >= 0 && kind < 13)
            {
                static const u32 kBpp[13] = {4, 3, 2, 2, 1, 1, 0, 0, 0, 4, 3, 2, 2};
                const uint64_t t1 = __rdtsc();
                ++s_calls[kind];
                s_bytes[kind] += (kBpp[kind] != 0u) ? static_cast<uint64_t>(count) * kBpp[kind] : static_cast<uint64_t>(count / 2u);
                s_ticks[kind] += t1 - t0;
            }
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - s_winT).count();
            if (ms >= 5000.0)
            {
                const uint64_t tsc = __rdtsc();
                const double tpm = static_cast<double>(tsc - s_winTsc) / ms;
                char buf[512];
                int len = ::snprintf(buf, sizeof(buf), "[uploadstat] window_ms=%.0f", ms);
                for (int k = 0; k < 13 && len > 0 && len < static_cast<int>(sizeof(buf)) - 64; ++k)
                {
                    if (s_calls[k] == 0)
                        continue;
                    len += ::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len), " kind=%d:%llu/%.2f/%.1f", k,
                                      static_cast<unsigned long long>(s_calls[k]), static_cast<double>(s_bytes[k]) / (1024.0 * 1024.0), static_cast<double>(s_ticks[k]) / tpm);
                    s_calls[k] = s_bytes[k] = s_ticks[k] = 0;
                    if (k == 4 && len > 0 && len < static_cast<int>(sizeof(buf)) - 48)
                        len += ::snprintf(buf + len, sizeof(buf) - static_cast<size_t>(len), " (fast=%llu groups=%llu)",
                                          static_cast<unsigned long long>(g_t8FastCalls.exchange(0, std::memory_order_relaxed)),
                                          static_cast<unsigned long long>(g_t8FastGroups.exchange(0, std::memory_order_relaxed)));
                }
                ::ps2xSafeLog("%s\n", buf);
                std::fflush(stderr);
                s_winT = now;
                s_winTsc = tsc;
            }
        }
        return ok;
    }

    // PERF-GR: read one row of pixels (same PixelStorageTraits<>::Read as ReadCT32/ReadCT24/ReadCT16/ReadCT16S, without the per-pixel indirect call).
    // kind: 0 CT32, 1 CT24, 2 CT16, 3 CT16S
    bool ReadRow(int kind, u8 *data, u32 bp, u32 bw, u32 x0, u32 y, u32 count, u32 *out)
    {
        switch (kind)
        {
        case 0:
            for (u32 i = 0; i < count; ++i)
                out[i] = PixelStorageTraits<C32>::Read(PageTableC32, data, bp, bw, x0 + i, y);
            return true;
        case 1:
            for (u32 i = 0; i < count; ++i)
                out[i] = PixelStorageTraits<C24>::Read(PageTableC32, data, bp, bw, x0 + i, y);
            return true;
        case 2:
            for (u32 i = 0; i < count; ++i)
                out[i] = PixelStorageTraits<C16>::Read(PageTableC16, data, bp, bw, x0 + i, y);
            return true;
        case 3:
            for (u32 i = 0; i < count; ++i)
                out[i] = PixelStorageTraits<C16S>::Read(PageTableC16S, data, bp, bw, x0 + i, y);
            return true;
        default:
            return false;
        }
    }
}


