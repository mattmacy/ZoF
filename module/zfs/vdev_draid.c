/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2018 Intel Corporation.
 * Copyright (c) 2020 by Lawrence Livermore National Security, LLC.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_draid.h>
#include <sys/vdev_raidz.h>
#include <sys/vdev_rebuild.h>
#include <sys/abd.h>
#include <sys/zio.h>
#include <sys/nvpair.h>
#include <sys/zio_checksum.h>
#include <sys/fs/zfs.h>
#include <sys/fm/fs/zfs.h>
#include <zfs_fletcher.h>

#ifdef ZFS_DEBUG
#include <sys/vdev.h>	/* For vdev_xlate() in vdev_draid_io_verify() */
#endif

/*
 * dRAID is a distributed spare implementation for ZFS. A dRAID vdev is
 * comprised of multiple raidz redundancy groups which are spread over the
 * dRAID children. To ensure an even distribution, and avoid hot spots, a
 * permutation mapping is applied to the order of the dRAID children.
 * This mixing effectively distributes the parity columns evenly over all
 * of the disks in the dRAID.
 *
 * This is beneficial because it means when resilvering all of the disks
 * can participate thereby increasing the available IOPs and bandwidth.
 * Furthermore, by reserving a small fraction of each child's total capacity
 * virtual distributed spare disks can be created. These spares similarly
 * benefit from the performance gains of spanning all of the children. The
 * consequence of which is that resilvering to a distributed spare can
 * substantially reduce the time required to restore full parity to pool
 * with a failed disks.
 *
 * === dRAID group layout ===
 *
 * First, let's define a "row" in the configuration to be a 16M chunk from
 * each physical drive at the same offset. This is the minimum allowable
 * size since it must be possible to store a full 16M block when there is
 * only a single data column. Next, we define a "group" to be a set of
 * sequential disks containing both the parity and data columns. We allow
 * groups to span multiple rows in order to align any group size to any
 * number of physical drives. Finally, a "slice" is comprised of the rows
 * which contain the target number of groups. The permutation mappings
 * are applied in a round robin fashion to each slice.
 *
 * Given n drives in a group (including parity drives) and m physical drives
 * (not including the spare drives), we can distribute the groups across r
 * rows without remainder by selecting the least common multiple of n and m
 * as the number of groups; i.e. ngroups = LCM(n, m).
 *
 * In the example below, there are 14 physical drives in the configuration
 * with two drives worth of spare capacity. Each group has a width of 9
 * which includes 8 data and 1 parity drive.  There are 4 groups and 3 rows
 * per slice.  Each group has a size of 144M (16M * 9) and a slice size is
 * 576M (144M * 4). When allocating from a dRAID each group is filled before
 * moving on to the next as show in slice0 below.
 *
 *
 *             data disks (8 data + 1 parity)          spares (2)
 *     +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *  ^  | 2 | 6 | 1 | 11| 4 | 0 | 7 | 10| 8 | 9 | 13| 5 | 12| 3 | device map 0
 *  |  +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *  |  |              group 0              |  group 1..|       |
 *  |  +-----------------------------------+-----------+-------|
 *  |  | 0   1   2   3   4   5   6   7   8 | 36  37  38|       |  r
 *  |  | 9   10  11  12  13  14  15  16  17| 45  46  47|       |  o
 *  |  | 18  19  20  21  22  23  24  25  26| 54  55  56|       |  w
 *     | 27  28  29  30  31  32  33  34  35| 63  64  65|       |  0
 *  s  +-----------------------+-----------------------+-------+
 *  l  |       ..group 1       |        group 2..      |       |
 *  i  +-----------------------+-----------------------+-------+
 *  c  | 39  40  41  42  43  44| 72  73  74  75  76  77|       |  r
 *  e  | 48  49  50  51  52  53| 81  82  83  84  85  86|       |  o
 *  0  | 57  58  59  60  61  62| 90  91  92  93  94  95|       |  w
 *     | 66  67  68  69  70  71| 99 100 101 102 103 104|       |  1
 *  |  +-----------+-----------+-----------------------+-------+
 *  |  |..group 2  |            group 3                |       |
 *  |  +-----------+-----------+-----------------------+-------+
 *  |  | 78  79  80|108 109 110 111 112 113 114 115 116|       |  r
 *  |  | 87  88  89|117 118 119 120 121 122 123 124 125|       |  o
 *  |  | 96  97  98|126 127 128 129 130 131 132 133 134|       |  w
 *  v  |105 106 107|135 136 137 138 139 140 141 142 143|       |  2
 *     +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *     | 9 | 11| 12| 2 | 4 | 1 | 3 | 0 | 10| 13| 8 | 5 | 6 | 7 | device map 1
 *  s  +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *  l  |              group 4              |  group 5..|       | row 3
 *  i  +-----------------------+-----------+-----------+-------|
 *  c  |       ..group 5       |        group 6..      |       | row 4
 *  e  +-----------+-----------+-----------------------+-------+
 *  1  |..group 6  |            group 7                |       | row 5
 *     +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *     | 3 | 5 | 10| 8 | 6 | 11| 12| 0 | 2 | 4 | 7 | 1 | 9 | 13| device map 2
 *  s  +===+===+===+===+===+===+===+===+===+===+===+===+===+===+
 *  l  |              group 8              |  group 9..|       | row 6
 *  i  +-----------------------------------------------+-------|
 *  c  |       ..group 9       |        group 10..     |       | row 7
 *  e  +-----------------------+-----------------------+-------+
 *  2  |..group 10 |            group 11               |       | row 8
 *     +-----------+-----------------------------------+-------+
 *
 * This layout has several advantages over requiring that each row contain
 * a whole number of groups.
 *
 * 1. The group count is not a relevant parameter when defining a dRAID
 *    layout. Only the group width is needed, and *all* groups will have
 *    the desired size.
 *
 * 2. All possible group widths (<= physical disk count) can be supported.
 *
 * 3. The logic within vdev_draid.c is simplified when the group width is
 *    the same for all groups (although some of the logic around computing
 *    permutation numbers and drive offsets is more complicated).
 *
 * N.B. The following array describes all valid dRAID permutation maps.
 * Each row is used to generate a permutation map for a different number
 * of children from a unique seed. The seeds were generated and carefully
 * evaluated by the 'draid' utility in order to provide balanced mappings.
 * In addition to the seed a checksum of the in-memory mapping is stored
 * for verification.
 *
 * The imbalance ratio of a given failure (e.g. 5 disks wide, child 3 failed,
 * with a given permutation map) is the ratio of the amounts of I/O that will
 * be sent to the least and most busy disks when resilvering. The average
 * imbalance ratio (of a given number of disks and permutation map) is the
 * average of the ratios of all possible single and double disk failures.
 *
 * In order to achieve a low ratio the number of rows in the mapping must be
 * significantly larger than the number of children. For dRAID the number of
 * rows has been limited to 256 to minimize the map size. This does result
 * in a gradually increasing imbalance ratio as seen in the table below.
 * Increasing the number of rows for larger child counts would reduce the
 * imbalance ratio. However, in practice when there are a large number of
 * children each child is responsible for fewer total IOs so it's less of
 * a concern.
 *
 * Note these values are hard coded and must never be changed.  Existing
 * pools depend on the same mapping always being generated in order to
 * read and write from the correct locations.  Any change would make
 * existing pools completely inaccessible.
 */
static draid_map_t draid_maps[VDEV_DRAID_MAX_MAPS] = {
	{   2, 256, 0xd27b123486e72fe2, 0x000000003848433d },	/* 1.000 */
	{   3, 256, 0x625f944e90fc7b1f, 0x00000000a8bfd5c4 },	/* 1.000 */
	{   4, 256, 0xc9ea9ec82340c885, 0x00000001819d7c69 },	/* 1.000 */
	{   5, 256, 0xf46733b7f4d47dfd, 0x00000002a1648d74 },	/* 1.010 */
	{   6, 256, 0x88c3c62d8585b362, 0x00000003d3b0c2c4 },	/* 1.031 */
	{   7, 256, 0xb60bf1766a5ae0bd, 0x0000000532571d69 },	/* 1.043 */
	{   8, 256, 0xe98930e3c5d2e90a, 0x00000006edfb0329 },	/* 1.059 */
	{   9, 256, 0x5a5430036b982ccb, 0x00000008ceaf6934 },	/* 1.056 */
	{  10, 256, 0x835aa99465b2144e, 0x0000000b5e2e3164 },	/* 1.087 */
	{  11, 256, 0x74ccebf1dcf3ae80, 0x0000000dd691358c },	/* 1.083 */
	{  12, 256, 0x1066c9233dd86924, 0x000000108eb93aaf },	/* 1.096 */
	{  13, 256, 0x7481b56debf0e637, 0x0000001424121fe4 },	/* 1.100 */
	{  14, 256, 0x559b8c44065f8967, 0x00000016ab2ff079 },	/* 1.121 */
	{  15, 256, 0x34c49545a2ee7f01, 0x0000001a6028efd6 },	/* 1.103 */
	{  16, 256, 0x4ebc50d1ac2e964f, 0x0000001db337b2bd },	/* 1.104 */
	{  17, 256, 0xb25b240b051dcfe0, 0x000000219d7efc4e },	/* 1.140 */
	{  18, 256, 0x79606dfe4b053b1f, 0x0000002680164399 },	/* 1.128 */
	{  19, 256, 0x892e343f2f31d690, 0x00000029eb392835 },	/* 1.130 */
	{  20, 256, 0x7a98ffad8a39b449, 0x0000002fe8fe2087 },	/* 1.148 */
	{  21, 256, 0x4b3cbabf9cfb1d0f, 0x00000036363a2408 },	/* 1.139 */
	{  22, 256, 0xf45c77abb4f035d4, 0x00000038dd0f3e84 },	/* 1.150 */
	{  23, 256, 0x541b50c5ff1b281b, 0x0000003f6a371b02 },	/* 1.173 */
	{  24, 256, 0xab0666c148ed3a60, 0x0000004583a52f77 },	/* 1.173 */
	{  25, 256, 0xd82c5eaad94c5e5b, 0x0000004c40869188 },	/* 1.188 */
	{  26, 256, 0x3a42dfda4eb880f7, 0x000000522c719bba },	/* 1.226 */
	{  27, 256, 0xd200d2fc6b54bf60, 0x0000005760b4fdf5 },	/* 1.228 */
	{  28, 256, 0xaf07d893ffd1986e, 0x0000005e0dc49ab0 },	/* 1.230 */
	{  29, 256, 0xc761779e63cd762f, 0x00000067be3cd85c },	/* 1.239 */
	{  30, 256, 0xca577b1e07f85ca5, 0x0000006f5517f3e4 },	/* 1.238 */
	{  31, 256, 0xfd50a593c518b3d4, 0x0000007370e7778f },	/* 1.273 */
	{  32, 256, 0x220c7a6cb145fd23, 0x0000007d9d9fa78f },	/* 1.293 */
	{  33, 256, 0xeebbb3d6d40970a5, 0x00000083a14e3e60 },	/* 1.297 */
	{  34, 256, 0xc94fe19955410228, 0x0000008f63355eac },	/* 1.316 */
	{  35, 256, 0xb3657369900a545c, 0x00000095a7c566eb },	/* 1.313 */
	{  36, 256, 0x1d1fa86e430aed40, 0x0000009cff7669fb },	/* 1.307 */
	{  37, 256, 0x41d4567a236661cb, 0x000000a7d66b278b },	/* 1.377 */
	{  38, 256, 0x72876b9ff093b21c, 0x000000ae9bc47f33 },	/* 1.396 */
	{  39, 256, 0xf5a7e1ea513951c2, 0x000000bcb616da83 },	/* 1.362 */
	{  40, 256, 0x1f86f0f407867aad, 0x000000c30e0445f3 },	/* 1.371 */
	{  41, 256, 0xc70c00ed99f77eae, 0x000000cd23b394fd },	/* 1.424 */
	{  42, 256, 0x47597ce12c6de3f5, 0x000000d7a3ac5add },	/* 1.416 */
	{  43, 256, 0x7257467388cb31e6, 0x000000e266068ab0 },	/* 1.438 */
	{  44, 256, 0xe36feeacae79ea7a, 0x000000eeac6dc5e6 },	/* 1.462 */
	{  45, 256, 0x57f3441d83fb9eb9, 0x000000f5f65de1b5 },	/* 1.438 */
	{  46, 256, 0xcb89e7b41fcfede7, 0x000001032761176b },	/* 1.449 */
	{  47, 256, 0x1d893b5b937e5aea, 0x00000117017c4b5c },	/* 1.512 */
	{  48, 256, 0x2878979d4c91c493, 0x000001183c88612d },	/* 1.472 */
	{  49, 256, 0x63f19c2ce78edeee, 0x000001296ed0ee44 },	/* 1.458 */
	{  50, 256, 0x1e1d40408bc716aa, 0x00000134cff620b1 },	/* 1.538 */
	{  51, 256, 0x2fcb046eeb1f207b, 0x0000013f67caf09c },	/* 1.543 */
	{  52, 256, 0x51d9ee3ca622717f, 0x0000014c447c9d87 },	/* 1.513 */
	{  53, 256, 0x35e35cb929826075, 0x0000015ba72c76c0 },	/* 1.573 */
	{  54, 256, 0x3a9ec2b0829222c9, 0x00000168979646be },	/* 1.549 */
	{  55, 256, 0xd955efca98a311df, 0x000001789b9cce52 },	/* 1.585 */
	{  56, 256, 0x445d2f84ade3469f, 0x0000018564732e7d },	/* 1.614 */
	{  57, 256, 0x26b57da7b1e97273, 0x0000019531d42382 },	/* 1.571 */
	{  58, 256, 0xdf7a90179e22dd3f, 0x0000019e491ef47f },	/* 1.636 */
	{  59, 256, 0xe032972b59b70972, 0x000001acac08341f },	/* 1.621 */
	{  60, 256, 0xb343e4cd3d287ddc, 0x000001bb444b5e46 },	/* 1.618 */
	{  61, 256, 0xd8d4e54c3df7e3a7, 0x000001c58fcda563 },	/* 1.726 */
	{  62, 256, 0x44334cc530fb29ba, 0x000001dc18d75844 },	/* 1.739 */
	{  63, 256, 0x65ad35d57c47f507, 0x000001ecae361bba },	/* 1.696 */
	{  64, 256, 0x2a3825f8c282e99f, 0x000001f84a07afec },	/* 1.659 */
	{  65, 256, 0x834c9d0d3597a504, 0x0000020bfd6d436c },	/* 1.758 */
	{  66, 256, 0x1d9e7b06f6c07a10, 0x0000021ea362bb87 },	/* 1.717 */
	{  67, 256, 0x6cc1b2e96739fa55, 0x000002265cdb7cce },	/* 1.742 */
	{  68, 256, 0xcfe89dfa4292bc17, 0x00000233104ac39b },	/* 1.771 */
	{  69, 256, 0x438becb1fd00d4c2, 0x000002505926acb4 },	/* 1.784 */
	{  70, 256, 0xf5b7e58a298b866c, 0x0000025bbc74fbed },	/* 1.776 */
	{  71, 256, 0x0f43ba704002fc93, 0x000002736934b7f3 },	/* 1.788 */
	{  72, 256, 0xf21c038144492c6f, 0x0000027ccabc9669 },	/* 1.821 */
	{  73, 256, 0xe3ab5428b9f7df94, 0x00000292e4ee9451 },	/* 1.738 */
	{  74, 256, 0x2b81da6ec6a9963d, 0x000002a3e4435d6c },	/* 1.894 */
	{  75, 256, 0xf40420342b450c83, 0x000002c30448b817 },	/* 1.758 */
	{  76, 256, 0x7ce590e7e8817733, 0x000002cdfca4e1d9 },	/* 1.781 */
	{  77, 256, 0x663670846e05bb4b, 0x000002dfec572132 },	/* 1.933 */
	{  78, 256, 0xa19572c41899d080, 0x000002ed12dd46a0 },	/* 1.921 */
	{  79, 256, 0x5e07613ecf057f41, 0x0000030aed6e6447 },	/* 1.894 */
	{  80, 256, 0xf4595de38313a5d3, 0x000003159f7397a1 },	/* 1.912 */
	{  81, 256, 0xc54089d7d084125a, 0x0000033234b59ff5 },	/* 1.976 */
	{  82, 256, 0xf908340da38c477b, 0x00000339d35d1583 },	/* 1.991 */
	{  83, 256, 0xcfcded7072046406, 0x000003504c96061c },	/* 1.987 */
	{  84, 256, 0x2af7e558a7e0f844, 0x000003705d412574 },	/* 1.911 */
	{  85, 256, 0x37eb43e6bf49f751, 0x0000037f68370ad3 },	/* 1.976 */
	{  86, 256, 0x99de847b1bb599b0, 0x0000039721fa3c62 },	/* 2.081 */
	{  87, 256, 0x23688c8037026ffd, 0x000003af9d3e8d8f },	/* 1.969 */
	{  88, 256, 0x3eb1120addbc60c1, 0x000003c441d3ee37 },	/* 2.020 */
	{  89, 256, 0x7e9a8a06b63f9603, 0x000003d7ab303470 },	/* 1.962 */
	{  90, 256, 0xd6f6f1850d1119c6, 0x000003e87888f4d2 },	/* 2.067 */
	{  91, 256, 0x16946b638e95845b, 0x000004091e6b0f69 },	/* 2.094 */
	{  92, 256, 0x2bc491717f9cd131, 0x0000042146e172aa },	/* 2.256 */
	{  93, 256, 0x054affaef1562f3b, 0x0000042f674b14cc },	/* 2.030 */
	{  94, 256, 0x54375dde674a6684, 0x0000044c0df12ea6 },	/* 2.184 */
	{  95, 256, 0xa052855253694818, 0x000004664c08a41f },	/* 2.077 */
	{  96, 256, 0xfc0849afa9f3604a, 0x00000479b7cefede },	/* 2.185 */
	{  97, 256, 0x2908de4f98003934, 0x0000048c02c0806e },	/* 2.079 */
	{  98, 256, 0xf8be7e271d7e53b5, 0x0000049e9e828659 },	/* 2.415 */
	{  99, 256, 0x1b9435fdab22a5dd, 0x000004c6070139f9 },	/* 1.984 */
	{ 100, 256, 0x2a17c2b63f3943e1, 0x000004da13183b24 },	/* 2.341 */
	{ 101, 256, 0x8ae2ee0facdb9938, 0x000004ec59eb8413 },	/* 2.181 */
	{ 102, 256, 0x583c2f6cded9d3a9, 0x0000050d25afb497 },	/* 2.387 */
	{ 103, 256, 0x93a173e7214e3dfa, 0x0000051ad37854d9 },	/* 2.163 */
	{ 104, 256, 0x78af3e86fccdbc29, 0x0000053f32a84d94 },	/* 2.497 */
	{ 105, 256, 0x03367c2f007f7dac, 0x00000552d02bff16 },	/* 2.121 */
	{ 106, 256, 0x6fbce373324789ec, 0x00000577c4e9b8ee },	/* 2.525 */
	{ 107, 256, 0x93e4e36a6e6e1902, 0x0000058f22ad9b3d },	/* 2.393 */
	{ 108, 256, 0xbad08bd583345655, 0x000005a22c650669 },	/* 2.497 */
	{ 109, 256, 0xc3e137ae1dbe8f41, 0x000005d1e236f82c },	/* 2.226 */
	{ 110, 256, 0x0f55a3fe5723ea92, 0x000005d7e3592444 },	/* 2.586 */
	{ 111, 256, 0xa55f7f8bdf9a66cf, 0x000005f1c8b42e4e },	/* 2.284 */
	{ 112, 256, 0xa42b5f8c23f7a65c, 0x00000614209d4444 },	/* 2.601 */
	{ 113, 256, 0xe04327a36da3c095, 0x000006409793dc82 },	/* 2.406 */
	{ 114, 256, 0x5e1c0cafcaff22c5, 0x0000063cb330ca51 },	/* 2.744 */
	{ 115, 256, 0x947eeebeaa418c7b, 0x0000067de838040c },	/* 2.295 */
	{ 116, 256, 0x827a7e53c45fd591, 0x00000691654028c2 },	/* 2.663 */
	{ 117, 256, 0xee6c6422508b8081, 0x000006c73cd1f5ca },	/* 2.455 */
	{ 118, 256, 0x8d10f85f77136c9b, 0x000006b780c28a86 },	/* 2.795 */
	{ 119, 256, 0x3ac37b68ece309f7, 0x000006dc2a3372d5 },	/* 2.482 */
	{ 120, 256, 0xfac222ae91b52d75, 0x000006fa4da340cd },	/* 2.784 */
	{ 121, 256, 0x63f33b583c0f2798, 0x0000071d247c5f54 },	/* 2.405 */
	{ 122, 256, 0x615c622935825616, 0x000007430c7176b3 },	/* 3.054 */
	{ 123, 256, 0xc69189d76872af9a, 0x0000075925c749d5 },	/* 2.500 */
	{ 124, 256, 0xf4050a2ff3986a42, 0x000007760b16d276 },	/* 2.781 */
	{ 125, 256, 0xcff6bf9171a277cb, 0x000007abf7457004 },	/* 2.714 */
	{ 126, 256, 0xa13c261de2a975d7, 0x000007b4edf43211 },	/* 2.880 */
	{ 127, 256, 0xc5f4031a6cec6b01, 0x000007deec966f87 },	/* 2.495 */
	{ 128, 256, 0x698d21f61befa7d4, 0x000007e95cbcb124 },	/* 3.133 */
	{ 129, 256, 0x2be63bbe59df8854, 0x0000081eba81b449 },	/* 2.658 */
	{ 130, 256, 0x2180fdc70ba19fbe, 0x00000840a86f275a },	/* 2.933 */
	{ 131, 256, 0x3c7b47190d7bca47, 0x0000085843c4ec0f },	/* 2.700 */
	{ 132, 256, 0xd06a2656c2b16a2d, 0x00000878dce5cdd6 },	/* 3.148 */
	{ 133, 256, 0x89dc1fb8baa12726, 0x00000894d45cfe9f },	/* 2.660 */
	{ 134, 256, 0x6615e50866192f13, 0x000008b110406a7d },	/* 3.212 */
	{ 135, 256, 0xa609c9f54b9dbf7f, 0x000008f64bbfa0cd },	/* 2.805 */
	{ 136, 256, 0x8fb485f7b8431419, 0x000008fc79ddf5ad },	/* 2.964 */
	{ 137, 256, 0x40988bde38cfae15, 0x0000090e944fe9a3 },	/* 3.059 */
	{ 138, 256, 0x76f1fb825f1b5f3b, 0x000009393a6b2604 },	/* 3.293 */
	{ 139, 256, 0xb1768315ba1ef1c1, 0x00000977ee6bb60b },	/* 2.667 */
	{ 140, 256, 0x947aebd113c16275, 0x000009995197900c },	/* 3.665 */
	{ 141, 256, 0xebd7e73fcbfbd250, 0x000009941f7d6a10 },	/* 3.027 */
	{ 142, 256, 0xc7c62d687efa04ba, 0x000009f1e7320726 },	/* 3.381 */
	{ 143, 256, 0x2b97bc1ac9bfc727, 0x000009dda86e488a },	/* 3.267 */
	{ 144, 256, 0x71a4c7a0d1b93bca, 0x00000a0ff5c6206a },	/* 3.141 */
	{ 145, 256, 0x3db0fd9a2889f2d3, 0x00000a3d5f8029a0 },	/* 2.903 */
	{ 146, 256, 0x5e16a0936e6ebb4f, 0x00000a61cfc44f33 },	/* 3.685 */
	{ 147, 256, 0x48d86513d51d5ab3, 0x00000a7a917df789 },	/* 3.076 */
	{ 148, 256, 0x0e2707c29c7c80f7, 0x00000ab8b21b090f },	/* 3.405 */
	{ 149, 256, 0xeef6b90b2873078e, 0x00000ad819b5f793 },	/* 3.390 */
	{ 150, 256, 0x5c74901930f42aa5, 0x00000b04bc34b61c },	/* 3.510 */
	{ 151, 256, 0x6780b9b7ef3d1571, 0x00000b13f0ac119c },	/* 3.176 */
	{ 152, 256, 0x5f9f45931955b101, 0x00000b3752cb069a },	/* 3.847 */
	{ 153, 256, 0x3988cd9403516c78, 0x00000b672b9f93c8 },	/* 3.143 */
	{ 154, 256, 0x6e3215639bb8405c, 0x00000b9567de82c9 },	/* 3.379 */
	{ 155, 256, 0x45056fbc5e5f8730, 0x00000bc2ba15e24d },	/* 3.616 */
	{ 156, 256, 0x46049b760054472d, 0x00000bcdec26b3c9 },	/* 3.450 */
	{ 157, 256, 0xbef6de70a79f0a75, 0x00000c2bd37f93e7 },	/* 3.625 */
	{ 158, 256, 0xb3c5c3db7c9794d0, 0x00000c3e23f9ed4e },	/* 3.690 */
	{ 159, 256, 0x352d2822beba6d5c, 0x00000c610d231c88 },	/* 3.415 */
	{ 160, 256, 0xf30ee19ddd4afa2e, 0x00000c6a6b246e6d },	/* 3.329 */
	{ 161, 256, 0xce68dd4ab2dcd278, 0x00000caeba617e2d },	/* 3.673 */
	{ 162, 256, 0x613c9e78805e41cb, 0x00000cbc2b0c61c2 },	/* 3.628 */
	{ 163, 256, 0xeeab63f6eaebae4d, 0x00000cfcb0895d26 },	/* 3.471 */
	{ 164, 256, 0x8bb8428ee5865272, 0x00000d2f9a8768a3 },	/* 3.873 */
	{ 165, 256, 0xfe06cfee48df11fa, 0x00000d5f4bc2b0e3 },	/* 3.646 */
	{ 166, 256, 0xcfd6e29926b59b14, 0x00000d6393bc05ee },	/* 3.345 */
	{ 167, 256, 0x4ffb773628a1e28d, 0x00000da911be9d37 },	/* 3.884 */
	{ 168, 256, 0x54505b3532af3810, 0x00000db8492201d0 },	/* 3.290 */
	{ 169, 256, 0x81cabcc02e8336f1, 0x00000e0420e97916 },	/* 3.391 */
	{ 170, 256, 0x7303ecfd5788a7b0, 0x00000e0934cfca6f },	/* 4.153 */
	{ 171, 256, 0xd6d187fcca63bc41, 0x00000e526875d3ed },	/* 3.661 */
	{ 172, 256, 0x12b3d6b7cf93198e, 0x00000e5cc7e5dfb3 },	/* 3.448 */
	{ 173, 256, 0x68b87e58537cb3ed, 0x00000e9322810a09 },	/* 4.091 */
	{ 174, 256, 0xe592972360b1f188, 0x00000ec9c33a5ed1 },	/* 3.520 */
	{ 175, 256, 0x42226d7740fd95d5, 0x00000ede204b3329 },	/* 3.729 */
	{ 176, 256, 0x85e79ec390f0c4ce, 0x00000f1174074484 },	/* 4.020 */
	{ 177, 256, 0xfa0f8f8c35fcc819, 0x00000f3f1ad39a3e },	/* 3.873 */
	{ 178, 256, 0x990fc6d5576461c7, 0x00000f87974caba0 },	/* 3.763 */
	{ 179, 256, 0x356eb43b1804de5f, 0x00000f9f2474d35e },	/* 4.185 */
	{ 180, 256, 0x38aa9000d7aae573, 0x00000fd5b6addd06 },	/* 3.432 */
	{ 181, 256, 0x0b1763e2e5eebd1d, 0x00000ffb76ce2b66 },	/* 4.008 */
	{ 182, 256, 0xaed65bed47dedd57, 0x0000101ac344590c },	/* 4.458 */
	{ 183, 256, 0x77e4fbca8c7fd444, 0x0000105d9c2a52c7 },	/* 3.891 */
	{ 184, 256, 0x9bcd3c6860f00181, 0x00001097462ff6f1 },	/* 3.613 */
	{ 185, 256, 0x5b7f5b92a8f38b96, 0x00001097827236eb },	/* 4.392 */
	{ 186, 256, 0x4ec22016d2d85110, 0x000010f77854adf5 },	/* 3.734 */
	{ 187, 256, 0x8d4cfc15d3f88d91, 0x000010f75120b900 },	/* 4.087 */
	{ 188, 256, 0x52f131b1250220e8, 0x00001158dfe4a41c },	/* 4.094 */
	{ 189, 256, 0xfa5dc1ee85fdebd7, 0x00001149e3d8e4af },	/* 4.044 */
	{ 190, 256, 0xcc6e84d8c990a8a9, 0x00001198c52212c5 },	/* 3.742 */
	{ 191, 256, 0xaece605d95d3a751, 0x000011bced5821f2 },	/* 4.611 */
	{ 192, 256, 0x936556ede86f0b85, 0x000011fb9c0b240f },	/* 3.838 */
	{ 193, 256, 0x22d3eb1a6eca886f, 0x00001231dbd85c54 },	/* 4.038 */
	{ 194, 256, 0x0d64a83435ee5147, 0x0000126ae7594a62 },	/* 4.505 */
	{ 195, 256, 0x603fc435f11781d7, 0x0000129d389a1f8b },	/* 4.287 */
	{ 196, 256, 0x5d25211ece491c0c, 0x000012c86c7bdc51 },	/* 3.792 */
	{ 197, 256, 0x316ae4dd498cdb99, 0x0000130c14089adf },	/* 5.040 */
	{ 198, 256, 0x0689348fe03cffe5, 0x0000130705e0bac0 },	/* 4.022 */
	{ 199, 256, 0xb547ad5221c59950, 0x0000135046838094 },	/* 4.205 */
	{ 200, 256, 0x0d7c80c5dda4b4cb, 0x000013a3e7132632 },	/* 4.446 */
	{ 201, 256, 0x05d55e7d70bad126, 0x000013bff4c42026 },	/* 4.375 */
	{ 202, 256, 0x5b6b3399dbd2bcbd, 0x000013f7b202914b },	/* 4.302 */
	{ 203, 256, 0xdf46f56c41ea861d, 0x0000142091c0ba26 },	/* 4.746 */
	{ 204, 256, 0x6ab8a044718a698b, 0x00001469b02bb128 },	/* 3.838 */
	{ 205, 256, 0xfb2b742d05f54096, 0x0000146789357a4b },	/* 4.332 */
	{ 206, 256, 0x5879587e83e5dfcb, 0x000014c437258b0d },	/* 4.694 */
	{ 207, 256, 0x61b65616dd4d9288, 0x000014d43b401a1e },	/* 4.486 */
	{ 208, 256, 0x8c3722ddabd63083, 0x0000150ec78643b7 },	/* 3.937 */
	{ 209, 256, 0x75a0df47f4d66fd8, 0x00001539a49cd0dc },	/* 4.975 */
	{ 210, 256, 0x4160fa0f875155e9, 0x00001570785bcbe9 },	/* 4.128 */
	{ 211, 256, 0xabe7e685cbc9ce5c, 0x0000159de43925eb },	/* 4.569 */
	{ 212, 256, 0x8689a65aaa3c99c0, 0x000015fc66ccb6b9 },	/* 4.798 */
	{ 213, 256, 0xa802e731e8320896, 0x00001621628872f5 },	/* 4.622 */
	{ 214, 256, 0x9c2c6beb7a7b25bb, 0x00001655fe9367fa },	/* 4.228 */
	{ 215, 256, 0x6c2bff4eecf7e523, 0x000016a67633f2dd },	/* 6.459 */
	{ 216, 256, 0x633da96e9ccb7220, 0x000016c1857ad660 },	/* 4.029 */
	{ 217, 256, 0xed34dcf8d4fdc37d, 0x0000171ae5c143cb },	/* 5.211 */
	{ 218, 256, 0xce9e0e8470219fb9, 0x0000175c46f535dc },	/* 5.269 */
	{ 219, 256, 0x48e419f13839522f, 0x000017511618b253 },	/* 4.903 */
	{ 220, 256, 0xe83ce578a61a3e92, 0x0000178efe345d42 },	/* 4.016 */
	{ 221, 256, 0x792501128b8e7562, 0x000017f6395d7838 },	/* 4.996 */
	{ 222, 256, 0x3d3b033300746ffd, 0x000017f9dede6cf7 },	/* 4.739 */
	{ 223, 256, 0xaa42b54bd79b9b39, 0x00001835031bc4e1 },	/* 5.121 */
	{ 224, 256, 0xbe8d8bfee659c4ff, 0x0000186ecee4caec },	/* 4.601 */
	{ 225, 256, 0x0e4fd33344959bf5, 0x0000188b770105b1 },	/* 4.763 */
	{ 226, 256, 0xa6318818535bd977, 0x000018bf36dba228 },	/* 4.662 */
	{ 227, 256, 0x09a58d6ef4cd24a4, 0x00001946e00c3d0e },	/* 5.025 */
	{ 228, 256, 0xd5df92c1210a61e1, 0x00001955f284187d },	/* 4.198 */
	{ 229, 256, 0x2f9dad47ecbfb07f, 0x000019b445a00aa2 },	/* 5.021 */
	{ 230, 256, 0x50d1653470eb8009, 0x000019e275ecc423 },	/* 4.988 */
	{ 231, 256, 0x859b561d9909f1f5, 0x00001a0985e6b6e6 },	/* 4.851 */
	{ 232, 256, 0x6e4495e95ba570a6, 0x00001a4c9ec980c5 },	/* 4.746 */
	{ 233, 256, 0x104a5ae2c742cd87, 0x00001a9a1f4de4f7 },	/* 4.982 */
	{ 234, 256, 0xbf6e8f617885bb29, 0x00001adc9d0df84d },	/* 4.787 */
	{ 235, 256, 0xba9db9112d231b48, 0x00001b05370c313e },	/* 4.962 */
	{ 236, 256, 0xcc430d194996378a, 0x00001b5f09eb6ae4 },	/* 4.884 */
	{ 237, 256, 0x8a37e532dcb37264, 0x00001ba88015fa57 },	/* 5.176 */
	{ 238, 256, 0x137fc0b403b6691f, 0x00001bc98a59844c },	/* 4.737 */
	{ 239, 256, 0x4b52fd61f556ebf1, 0x00001bb4446eae57 },	/* 4.970 */
	{ 240, 256, 0xe151761a61bed245, 0x00001bfc708585e4 },	/* 4.860 */
	{ 241, 256, 0x18ad79678dcc175b, 0x00001c497759b280 },	/* 5.029 */
	{ 242, 256, 0x70d604fcd9499c33, 0x00001ca489da0135 },	/* 5.811 */
	{ 243, 256, 0x584678bd5bec7e6b, 0x00001cce5fb12f23 },	/* 5.022 */
	{ 244, 256, 0x3df107aa54b635b3, 0x00001d013be32dd7 },	/* 4.550 */
	{ 245, 256, 0xcc8377b324aa1922, 0x00001d33f9a376d2 },	/* 5.066 */
	{ 246, 256, 0xc189e45cb4aca673, 0x00001d609af1a280 },	/* 4.913 */
	{ 247, 256, 0xa2bf7a007477f3c5, 0x00001d9fefa22ca8 },	/* 5.500 */
	{ 248, 256, 0x8a9e55e3586eb6ab, 0x00001de182ca01ce },	/* 5.240 */
	{ 249, 256, 0x6d6feba1dcae9397, 0x00001e37f9906fc5 },	/* 5.180 */
	{ 250, 256, 0x889f6848d4489d14, 0x00001ea6fc12e456 },	/* 5.326 */
	{ 251, 256, 0x2126c3b4ee836dde, 0x00001ea151a0e96e },	/* 4.989 */
	{ 252, 256, 0xceec65ee5be40279, 0x00001f08192ed5c1 },	/* 5.030 */
	{ 253, 256, 0x6d69532520419418, 0x00001f3c8e9b0b72 },	/* 5.389 */
	{ 254, 256, 0x8c93161db4f0fd85, 0x00001f79c5d08c45 },	/* 5.510 */
	{ 255, 256, 0xacd9a3be765cb85d, 0x00001fc35c2b6a2b },	/* 5.409 */
};

/*
 * Verify the map is valid.
 */
static int
check_map(draid_map_t *map)
{
	uint64_t children = map->dm_children;
	uint64_t nperms = map->dm_nperms;
	uint16_t *counts = kmem_zalloc(sizeof (uint16_t) * children, KM_SLEEP);

	/* Ensure each device index appears exactly once in every row */
	for (int i = 0; i < nperms; i++) {
		for (int j = 0; j < children; j++) {
			uint8_t val = map->dm_perms[(i * children) + j];

			if (val >= children || counts[val] != i) {
				kmem_free(counts, sizeof (uint16_t) * children);
				return (EINVAL);
			}

			counts[val]++;
		}
	}

	/* Verify checksum when provided by the map */
	if (map->dm_checksum != 0) {
		zio_cksum_t cksum;
		fletcher_4_native_varsize(map->dm_perms,
		    sizeof (uint8_t) * children * nperms, &cksum);

		if (map->dm_checksum != cksum.zc_word[0]) {
			kmem_free(counts, sizeof (uint16_t) * children);
			return (ECKSUM);
		}
	}

	kmem_free(counts, sizeof (uint16_t) * children);

	return (0);
}

/*
 * Allocate a permutation map.
 */
static draid_map_t *
alloc_map(uint64_t children, uint64_t nperms, uint64_t seed, uint64_t checksum)
{
	draid_map_t *map = kmem_alloc(sizeof (draid_map_t), KM_SLEEP);
	map->dm_seed = seed;
	map->dm_checksum = checksum;
	map->dm_children = children;
	map->dm_nperms = nperms;
	map->dm_perms = kmem_alloc(sizeof (uint8_t) *
	    map->dm_children * map->dm_nperms, KM_SLEEP);

	return (map);
}

/*
 * Free a permutation map.
 */
void
vdev_draid_free_map(draid_map_t *map)
{
	kmem_free(map->dm_perms, sizeof (uint8_t) *
	    map->dm_children * map->dm_nperms);
	kmem_free(map, sizeof (draid_map_t));
}

/*
 * Generate a permutation map from the seed and validate it against the
 * checksum when provided.  These maps control the placement of all data
 * in a dRAID.  Therefore it's critical that the map_seed always generates
 * the same map.  We provide our own pseudo-random number generator for
 * this purpose.
 */
int
vdev_draid_generate_map(uint64_t children, uint64_t map_seed,
    uint64_t checksum, uint64_t nperms, draid_map_t **mapp)
{
#ifdef _KERNEL
	/*
	 * The kernel code always provides both a map_seed and checksum.
	 * Only the tests/zfs-tests/cmd/draid/draid.c utility will provide
	 * a zero checksum when generating new candidate maps.
	 */
	VERIFY3U(children, >=, 2);
	VERIFY3U(children, <=, VDEV_DRAID_MAX_CHILDREN);
	VERIFY3U(map_seed, !=, 0);
	VERIFY3U(checksum, !=, 0);
#endif

	int rowsz = sizeof (uint8_t) * children;
	draid_map_t *map = alloc_map(children, nperms, map_seed, checksum);

	/* Setup an initial row with a known pattern */
	uint8_t *initial_row = kmem_alloc(rowsz, KM_SLEEP);
	for (int i = 0; i < children; i++)
		initial_row[i] = i;

	uint64_t draid_seed[2] = { VDEV_DRAID_SEED, map_seed };
	uint8_t *current_row, *previous_row = initial_row;

	/*
	 * Perform a Fisher-Yates shuffle of each row using the previous
	 * row as the starting point.  An initial_row with known pattern
	 * is used as the input for the first row.
	 */
	for (int i = 0; i < nperms; i++) {
		current_row = &map->dm_perms[i * children];
		memcpy(current_row, previous_row, rowsz);

		for (int j = children - 1; j > 0; j--) {
			uint64_t k = vdev_draid_rand(draid_seed) % (j + 1);
			uint8_t val = current_row[j];
			current_row[j] = current_row[k];
			current_row[k] = val;
		}

		previous_row = current_row;
	}

	kmem_free(initial_row, rowsz);

	int error = check_map(map);
	if (error) {
		vdev_draid_free_map(map);
		return (error);
	}

	*mapp = map;
	return (0);
}

/*
 * Lookup the map seed and checksum.
 */
int
vdev_draid_lookup_map(uint64_t children, uint64_t *map_seed,
    uint64_t *map_checksum, uint64_t *map_nperms)
{
	for (int i = 0; i <= VDEV_DRAID_MAX_MAPS; i++) {
		if (draid_maps[i].dm_children == children) {
			*map_seed = draid_maps[i].dm_seed;
			*map_checksum = draid_maps[i].dm_checksum;
			*map_nperms = draid_maps[i].dm_nperms;
			return (0);
		}
	}

	return (ENOENT);
}

/*
 * Lookup the permutation array and iteration id for the provided offset.
 */
static void
vdev_draid_get_perm(vdev_draid_config_t *vdc, uint64_t pindex,
    uint8_t **base, uint64_t *iter)
{
	uint64_t ncols = vdc->vdc_children;
	uint64_t poff = pindex % (vdc->vdc_map->dm_nperms * ncols);

	*base = vdc->vdc_map->dm_perms + (poff / ncols) * ncols;
	*iter = poff % ncols;
}

static inline uint64_t
vdev_draid_permute_id(vdev_draid_config_t *vdc,
    uint8_t *base, uint64_t iter, uint64_t index)
{
	return ((base[index] + iter) % vdc->vdc_children);
}

/*
 * Full stripe writes.  For "big columns" it's sufficient to map the correct
 * range of the zio ABD.  Partial columns require allocating a gang ABD in
 * order to zero fill the skip sectors.  When the column is empty a zero
 * filled skip sector must be mapped.  In all cases the data ABDs must be
 * the same size as the parity ABDs.
 *
 * Both rm->cols and rc->rc_size are increased to calculate the parity over
 * the full stripe width.  All zero filled skip sectors must be written to
 * disk.  They are read when performing a sequential resilver and used in
 * the parity calculation when performing reconstruction.
 */
static void
vdev_draid_map_alloc_write(zio_t *zio, raidz_map_t *rm)
{
	uint64_t skip_size = 1ULL << zio->io_vd->vdev_top->vdev_ashift;
	uint64_t parity_size = rm->rm_col[0].rc_size;
	uint64_t abd_off = 0;

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_WRITE);
	ASSERT3U(parity_size, ==, abd_get_size(rm->rm_col[0].rc_abd));

	for (uint64_t c = rm->rm_firstdatacol; c < rm->rm_scols; c++) {
		raidz_col_t *rc = &rm->rm_col[c];

		if (rm->rm_skipstart == 0 || c < rm->rm_skipstart) {
			/* this is a "big column */
			ASSERT3U(rc->rc_size, ==, parity_size);
			rc->rc_abd = abd_get_offset_size(zio->io_abd,
			    abd_off, rc->rc_size);
		} else if (c < rm->rm_cols) {
			/* short data column, add a skip sector */
			ASSERT3U(rc->rc_size + skip_size, ==, parity_size);
			rc->rc_abd = abd_alloc_gang_abd();
			abd_gang_add(rc->rc_abd, abd_get_offset_size(
			    zio->io_abd, abd_off, rc->rc_size), B_TRUE);
			abd_gang_add(rc->rc_abd, abd_get_zeros(skip_size),
			    B_TRUE);
		} else {
			ASSERT0(rc->rc_size);
			ASSERT3U(skip_size, ==, parity_size);
			/* empty data column (small write), add a skip sector */
			rc->rc_abd = abd_get_zeros(skip_size);
		}

		ASSERT3U(abd_get_size(rc->rc_abd), ==, parity_size);

		abd_off += rc->rc_size;
		rc->rc_size = parity_size;
	}
	ASSERT3U(abd_off, ==, zio->io_size);
	rm->rm_cols = rm->rm_scols;
}

/*
 * Scrub/resilver reads.  In order to store the contents of the skip sectors
 * an additional ABD is allocated.  The columns are handled in the same way
 * as a full stripe write except instead of using the zero ABD the newly
 * allocated skip ABD is used to back the skip sectors.  In all cases the
 * data ABD must be the same size as the parity ABDs.
 *
 * Both the rm->rm_cols and rc->rc_size are increased to allow the parity
 * to be calculated for the stripe.
 */
static void
vdev_draid_map_alloc_scrub(zio_t *zio, raidz_map_t *rm)
{
	uint64_t skip_size = 1ULL << zio->io_vd->vdev_top->vdev_ashift;
	uint64_t abd_off = 0;

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_READ);

	rm->rm_abd_skip = abd_alloc_linear(rm->rm_nskip * skip_size, B_TRUE);

	for (uint64_t c = rm->rm_firstdatacol; c < rm->rm_scols; c++) {
		raidz_col_t *rc = &rm->rm_col[c];
		int skip_idx = c - rm->rm_skipstart;

		if (rm->rm_skipstart == 0 || c < rm->rm_skipstart) {
			rc->rc_abd = abd_get_offset_size(zio->io_abd,
			    abd_off, rc->rc_size);
		} else if (c < rm->rm_cols) {
			rc->rc_abd = abd_alloc_gang_abd();
			abd_gang_add(rc->rc_abd, abd_get_offset_size(
			    zio->io_abd, abd_off, rc->rc_size), B_TRUE);
			abd_gang_add(rc->rc_abd, abd_get_offset_size(
			    rm->rm_abd_skip, skip_idx * skip_size, skip_size),
			    B_TRUE);
		} else {
			rc->rc_abd = abd_get_offset_size(rm->rm_abd_skip,
			    skip_idx * skip_size, skip_size);
		}

		uint64_t abd_size = abd_get_size(rc->rc_abd);
		ASSERT3U(abd_size, ==, abd_get_size(rm->rm_col[0].rc_abd));

		abd_off += rc->rc_size;
		rc->rc_size = abd_size;
	}

	rm->rm_cols = rm->rm_scols;
}

/*
 * Normal reads.  This is the common case, it is sufficient to map the zio's
 * ABD in to the raid map columns.  If the checksum cannot be verified the
 * raid map is expanded by vdev_draid_map_include_skip_sectors() to allow
 * reconstruction from parity data.
 */
static void
vdev_draid_map_alloc_read(zio_t *zio, raidz_map_t *rm)
{
	uint64_t abd_off = 0;

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_READ);

	for (uint64_t c = rm->rm_firstdatacol; c < rm->rm_cols; c++) {
		raidz_col_t *rc = &rm->rm_col[c];

		rc->rc_abd = abd_get_offset_size(zio->io_abd, abd_off,
		    rc->rc_size);
		abd_off += rc->rc_size;
	}
}

/*
 * Given a logical address within a dRAID configuration, return the physical
 * address on the first drive in the group that this address maps to
 * (at position 'start' in permutation number 'perm').
 */
static uint64_t
vdev_draid_logical_to_physical(vdev_t *vd, uint64_t logical_offset,
    uint64_t *perm, uint64_t *start)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	/* b is the dRAID (parent) sector offset. */
	uint64_t ashift = vd->vdev_top->vdev_ashift;
	uint64_t b_offset = logical_offset >> ashift;

	/*
	 * The size of a ROW in units of the vdev's minimum sector size.
	 * ROW is the amount of data written to each disk of each group
	 * in a given permutation.
	 */
	uint64_t blocks_per_row = VDEV_DRAID_ROWSIZE >> ashift;

	/*
	 * We cycle through a disk permutation every groupsz * ngroups chunk
	 * of address space. Note that ngroups * groupsz must be a multiple
	 * of the number of data drives (ndisks) in order to guarantee
	 * alignment. So, for example, if our row size is 16MB, our group
	 * size is 10, and there are 13 data drives in the draid, then ngroups
	 * will be 13, we will change permutation every 2.08GB and each
	 * disk will have 160MB of data per chunk.
	 */
	uint64_t groupwidth = vdc->vdc_groupwidth;
	uint64_t ngroups = vdc->vdc_ngroups;
	uint64_t ndisks = vdc->vdc_ndisks;

	/*
	 * groupstart is where the group this IO will land in "starts" in
	 * the permutation array.
	 */
	uint64_t group = logical_offset / vdc->vdc_groupsz;
	uint64_t groupstart = (group * groupwidth) % ndisks;
	ASSERT3U(groupstart + groupwidth, <=, ndisks + groupstart);
	*start = groupstart;

	/* b_offset is the sector offset within a group chunk */
	b_offset = b_offset % (blocks_per_row * groupwidth);
	ASSERT0(b_offset % groupwidth);

	/*
	 * Find the starting byte offset on each child vdev:
	 * - within a permutation there are ngroups groups spread over the
	 *   rows, where each row covers a slice portion of the disk
	 * - each permutation has (groupwidth * ngroups) / ndisks rows
	 * - so each permutation covers rows * slice portion of the disk
	 * - so we need to find the row where this IO group target begins
	 */
	*perm = group / ngroups;
	uint64_t row = (*perm * ((groupwidth * ngroups) / ndisks)) +
	    (((group % ngroups) * groupwidth) / ndisks);

	return (((blocks_per_row * row) + (b_offset / groupwidth)) << ashift);
}

/*
 * Allocate the raidz mapping to be applied to the dRAID I/O.  The parity
 * calculations for dRAID are identical to raidz.  The only caveat is that
 * dRAID always allocates a full stripe width.  Zero filled skip sectors
 * are added to pad out the buffer and must be written to disk.
 */
static raidz_map_t *
vdev_draid_map_alloc(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;

	/* Lookup starting byte offset on each child vdev */
	uint64_t groupstart, perm;
	uint64_t physical_offset = vdev_draid_logical_to_physical(vd,
	    zio->io_offset, &perm, &groupstart);

	/*
	 * If there is less than groupwidth drives available after the group
	 * start, the group is going to wrap onto the next row. 'wrap' is the
	 * group disk number that starts on the next row.
	 */
	vdev_draid_config_t *vdc = vd->vdev_tsd;
	uint64_t ndisks = vdc->vdc_ndisks;
	uint64_t groupwidth = vdc->vdc_groupwidth;
	uint64_t wrap = groupwidth;

	if (groupstart + groupwidth > ndisks)
		wrap = ndisks - groupstart;

	/* The zio's size in units of the vdev's minimum sector size. */
	const uint64_t ashift = vd->vdev_top->vdev_ashift;
	const uint64_t psize = zio->io_size >> ashift;

	/*
	 * "Quotient": The number of data sectors for this stripe on all but
	 * the "big column" child vdevs that also contain "remainder" data.
	 */
	uint64_t q = psize / vdc->vdc_ndata;

	/*
	 * "Remainder": The number of partial stripe data sectors in this I/O.
	 * This will add a sector to some, but not all, child vdevs.
	 */
	uint64_t r = psize - q * vdc->vdc_ndata;

	/* The number of "big columns" - those which contain remainder data. */
	uint64_t bc = (r == 0 ? 0 : r + vdc->vdc_nparity);
	ASSERT3U(bc, <, groupwidth);

	/* The total number of data and parity sectors for this I/O. */
	uint64_t tot = psize + (vdc->vdc_nparity * (q + (r == 0 ? 0 : 1)));

	raidz_map_t *rm = kmem_alloc(offsetof(raidz_map_t, rm_col[groupwidth]),
	    KM_SLEEP);

	rm->rm_cols = (q == 0) ? bc : groupwidth;
	rm->rm_scols = groupwidth;
	rm->rm_bigcols = bc;
	rm->rm_skipstart = bc;
	rm->rm_missingdata = 0;
	rm->rm_missingparity = 0;
	rm->rm_firstdatacol = vdc->vdc_nparity;
	rm->rm_abd_copy = NULL;
	rm->rm_abd_skip = NULL;
	rm->rm_reports = 0;
	rm->rm_freed = 0;
	rm->rm_ecksuminjected = 0;
	rm->rm_include_skip = 1;

	uint8_t *base;
	uint64_t iter, asize = 0;
	vdev_draid_get_perm(vdc, perm, &base, &iter);
	for (uint64_t i = 0; i < groupwidth; i++) {
		uint64_t c = (groupstart + i) % ndisks;

		/* increment the offset if we wrap to the next row */
		if (i == wrap)
			physical_offset += VDEV_DRAID_ROWSIZE;

		rm->rm_col[i].rc_devidx =
		    vdev_draid_permute_id(vdc, base, iter, c);
		rm->rm_col[i].rc_offset = physical_offset;
		rm->rm_col[i].rc_abd = NULL;
		rm->rm_col[i].rc_gdata = NULL;
		rm->rm_col[i].rc_error = 0;
		rm->rm_col[i].rc_tried = 0;
		rm->rm_col[i].rc_skipped = 0;
		rm->rm_col[i].rc_repair = 0;

		if (i >= rm->rm_cols)
			rm->rm_col[i].rc_size = 0;
		else if (i < bc)
			rm->rm_col[i].rc_size = (q + 1) << ashift;
		else
			rm->rm_col[i].rc_size = q << ashift;

		asize += rm->rm_col[i].rc_size;
	}

	ASSERT3U(asize, ==, tot << ashift);
	rm->rm_asize = roundup(asize, groupwidth << ashift);
	rm->rm_nskip = roundup(tot, groupwidth) - tot;
	IMPLY(bc > 0, rm->rm_nskip == groupwidth - bc);
	ASSERT3U(rm->rm_asize - asize, ==, rm->rm_nskip << ashift);
	ASSERT3U(rm->rm_nskip, <, vdc->vdc_ndata);

	/* Allocate buffers for the parity columns */
	for (uint64_t c = 0; c < rm->rm_firstdatacol; c++) {
		raidz_col_t *rc = &rm->rm_col[c];
		rc->rc_abd = abd_alloc_linear(rc->rc_size, B_TRUE);
	}

	/*
	 * Map buffers for data columns and allocate/map buffers for skip
	 * sectors.  There are three distinct cases for dRAID which are
	 * required to support sequential rebuild.
	 */
	if (zio->io_type == ZIO_TYPE_WRITE) {
		vdev_draid_map_alloc_write(zio, rm);
	} else if ((rm->rm_nskip > 0) &&
	    (zio->io_flags & (ZIO_FLAG_SCRUB | ZIO_FLAG_RESILVER))) {
		vdev_draid_map_alloc_scrub(zio, rm);
	} else {
		ASSERT3U(zio->io_type, ==, ZIO_TYPE_READ);
		vdev_draid_map_alloc_read(zio, rm);
	}

	rm->rm_ops = vdev_raidz_math_get_ops();
	zio->io_vsd = rm;
	zio->io_vsd_ops = &vdev_raidz_vsd_ops;

	return (rm);
}

/*
 * Converts a dRAID read raidz_map_t to a dRAID scrub raidz_map_t.  The
 * key difference is that an ABD is allocated to back up skip sectors
 * so they may be read, verified, and repaired if needed.
 */
void
vdev_draid_map_include_skip_sectors(zio_t *zio)
{
	raidz_map_t *rm = zio->io_vsd;

	ASSERT3U(zio->io_type, ==, ZIO_TYPE_READ);
	ASSERT3P(rm->rm_abd_skip, ==, NULL);

	for (uint64_t c = rm->rm_firstdatacol; c < rm->rm_cols; c++) {
		ASSERT(!abd_is_gang(rm->rm_col[c].rc_abd));
		abd_put(rm->rm_col[c].rc_abd);
	}

	vdev_draid_map_alloc_scrub(zio, rm);
}

/*
 * Converts a logical offset to the corresponding group number.
 */
uint64_t
vdev_draid_offset_to_group(vdev_t *vd, uint64_t offset)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	return (offset / vdc->vdc_groupsz);
}

/*
 * Converts a group number to the logical starting offset for that group.
 */
uint64_t
vdev_draid_group_to_offset(vdev_t *vd, uint64_t group)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	return (group * vdc->vdc_groupsz);
}

/*
 * Given a offset into a dRAID, compute a group aligned offset.
 */
uint64_t
vdev_draid_get_astart(vdev_t *vd, const uint64_t start)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	return (roundup(start, vdc->vdc_groupwidth << vd->vdev_ashift));
}

/*
 * Return the asize which is the psize rounded up to a full group width.
 * i.e. vdev_draid_psize_to_asize().
 */
static uint64_t
vdev_draid_asize(vdev_t *vd, uint64_t psize)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;
	uint64_t ashift = vd->vdev_ashift;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	uint64_t rows = ((psize - 1) / (vdc->vdc_ndata << ashift)) + 1;
	uint64_t asize = (rows * vdc->vdc_groupwidth) << ashift;

	ASSERT3U(asize, !=, 0);
	ASSERT3U(asize, <, vdc->vdc_groupsz);
	ASSERT3U(asize % (vdc->vdc_groupwidth), ==, 0);

	return (asize);
}

/*
 * Deflate the asize to the psize, this includes stripping parity.
 */
uint64_t
vdev_draid_asize_to_psize(vdev_t *vd, uint64_t asize)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT0(asize % vdc->vdc_groupwidth);

	return ((asize / vdc->vdc_groupwidth) * vdc->vdc_ndata);
}

/*
 * A dRAID spare does not fit into the DTL model. While it has child vdevs,
 * there is no redundancy among them, and the effective child vdev is
 * determined by offset. Moreover, DTLs of a child vdev before the spare
 * becomes active are invalid because the spare blocks were not in use yet.
 *
 * Here we are essentially doing a vdev_dtl_reassess() on the fly, by replacing
 * a dRAID spare with the child vdev under the offset. Note that it is a
 * recursive process because the child vdev can be another dRAID spare, and so
 * on.
 */
boolean_t
vdev_draid_missing(vdev_t *vd, uint64_t physical_offset, uint64_t txg,
    uint64_t size)
{
	if (vdev_dtl_contains(vd, DTL_MISSING, txg, size))
		return (B_TRUE);

	if (vd->vdev_ops == &vdev_draid_spare_ops) {
		vd = vdev_draid_spare_get_child(vd, physical_offset);
		if (vd == NULL)
			return (B_TRUE);
	}

	if (vd->vdev_ops != &vdev_spare_ops)
		return (vdev_dtl_contains(vd, DTL_MISSING, txg, size));

	if (vdev_dtl_contains(vd, DTL_MISSING, txg, size))
		return (B_TRUE);

	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		if (!vdev_readable(cvd))
			continue;

		if (!vdev_draid_missing(cvd, physical_offset, txg, size))
			return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Determine if the vdev is readable at the given offset.
 */
boolean_t
vdev_draid_readable(vdev_t *vd, uint64_t physical_offset)
{
	if (vd->vdev_ops == &vdev_draid_spare_ops) {
		vd = vdev_draid_spare_get_child(vd, physical_offset);
		if (vd == NULL)
			return (B_FALSE);
	}

	return (vdev_readable(vd));
}

/*
 * Returns the first distributed spare found under the provided vdev tree.
 */
static vdev_t *
vdev_draid_find_spare(vdev_t *vd)
{
	if (vd->vdev_ops == &vdev_draid_spare_ops)
		return (vd);

	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *svd = vdev_draid_find_spare(vd->vdev_child[c]);
		if (svd != NULL)
			return (svd);
	}

	return (NULL);
}

/*
 * Returns B_TRUE if the passed in vdev is currently "faulted".
 * Faulted, in this context, means that the vdev represents a
 * replacing or sparing vdev tree.
 */
static boolean_t
vdev_draid_faulted(vdev_t *vd, uint64_t physical_offset)
{
	if (vd->vdev_ops == &vdev_draid_spare_ops) {
		vd = vdev_draid_spare_get_child(vd, physical_offset);
		if (vd == NULL)
			return (B_FALSE);

		/*
		 * After resolving the distributed spare to a leaf vdev
		 * check the parent to determine if it's "faulted".
		 */
		vd = vd->vdev_parent;
	}

	return (vd->vdev_ops == &vdev_replacing_ops ||
	    vd->vdev_ops == &vdev_spare_ops);
}

/*
 * Determine if the dRAID block at the logical offset is degraded.
 */
static boolean_t
vdev_draid_group_degraded(vdev_t *vd, uint64_t offset)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	uint64_t groupstart, perm;
	uint64_t physical_offset = vdev_draid_logical_to_physical(vd,
	    offset, &perm, &groupstart);

	uint8_t *base;
	uint64_t iter;
	vdev_draid_get_perm(vdc, perm, &base, &iter);

	for (uint64_t i = 0; i < vdc->vdc_groupwidth; i++) {
		uint64_t c = (groupstart + i) % vdc->vdc_ndisks;
		uint64_t cid = vdev_draid_permute_id(vdc, base, iter, c);
		vdev_t *cvd = vd->vdev_child[cid];

		if (vdev_draid_faulted(cvd, physical_offset))
			return (B_TRUE);
	}

	return (B_FALSE);
}

/*
 * Create the vdev_draid_config_t structure from dRAID configuration stored
 * as an nvlist in the pool configuration.
 */
static vdev_draid_config_t *
vdev_draid_config_create(vdev_t *vd)
{
	vdev_draid_config_t *vdc = kmem_alloc(sizeof (*vdc), KM_SLEEP);

	/*
	 * Values read from the dRAID nvlist configuration.
	 */
	vdc->vdc_ndata = vd->vdev_ndata;
	vdc->vdc_nparity = vd->vdev_nparity;
	vdc->vdc_nspares = vd->vdev_nspares;
	vdc->vdc_children = vd->vdev_children;
	vdc->vdc_ngroups = vd->vdev_ngroups;

	uint64_t map_seed, map_checksum, map_nperms;
	int error = vdev_draid_lookup_map(vdc->vdc_children, &map_seed,
	    &map_checksum, &map_nperms);
	if (error) {
		kmem_free(vdc, sizeof (*vdc));
		return (NULL);
	}

	/*
	 * By passing in both a non-zero seed and checksum we are guaranteed
	 * the generated map's checksum will be verified.  This can never
	 * fail because all allowed seeds and checksums are hard coded in the
	 * draid_maps array and known to be correct.
	 */
	VERIFY3U(map_seed, !=, 0);
	VERIFY3U(map_checksum, !=, 0);

	error = vdev_draid_generate_map(vdc->vdc_children, map_seed,
	    map_checksum, map_nperms, &vdc->vdc_map);
	if (error) {
		kmem_free(vdc, sizeof (*vdc));
		return (NULL);
	}

	/*
	 * Derived constants.
	 */
	vdc->vdc_groupwidth = vdc->vdc_ndata + vdc->vdc_nparity;
	vdc->vdc_ndisks = vdc->vdc_children - vdc->vdc_nspares;
	vdc->vdc_groupsz = vdc->vdc_groupwidth * VDEV_DRAID_ROWSIZE;
	vdc->vdc_devslicesz = (vdc->vdc_groupsz * vdc->vdc_ngroups) /
	    vdc->vdc_ndisks;

	ASSERT3U(vdc->vdc_groupwidth, >=, 2);
	ASSERT3U(vdc->vdc_groupwidth, <=, vdc->vdc_ndisks);
	ASSERT3U(vdc->vdc_groupsz, >=, 2 * VDEV_DRAID_ROWSIZE);
	ASSERT3U(vdc->vdc_devslicesz, >=, VDEV_DRAID_ROWSIZE);
	ASSERT3U(vdc->vdc_devslicesz % VDEV_DRAID_ROWSIZE, ==, 0);
	ASSERT3U((vdc->vdc_groupwidth * vdc->vdc_ngroups) %
	    vdc->vdc_ndisks, ==, 0);

	return (vdc);
}

/*
 * Destroy the vdev_draid_config_t structure.
 */
static void
vdev_draid_config_destroy(vdev_draid_config_t *vdc)
{
	vdev_draid_free_map(vdc->vdc_map);
	kmem_free(vdc, sizeof (*vdc));
}

/*
 * Find the smallest child asize and largest sector size to calculate the
 * available capacity.  Distributed spares are ignored since their capacity
 * is also based of the minimum child size in the top-level dRAID.
 */
static void
vdev_draid_calculate_asize(vdev_t *vd, uint64_t *asizep,
    uint64_t *max_asizep, uint64_t *ashiftp)
{
	uint64_t asize = 0, max_asize = 0, ashift = 0;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	for (int c = 0; c < vd->vdev_children; c++) {
		vdev_t *cvd = vd->vdev_child[c];

		if (cvd->vdev_ops != &vdev_draid_spare_ops) {
			asize = MIN(asize - 1, cvd->vdev_asize - 1) + 1;
			max_asize = MIN(max_asize - 1,
			    cvd->vdev_max_asize - 1) + 1;
			ashift = MAX(ashift, cvd->vdev_ashift);
		}
	}

	*asizep = asize;
	*max_asizep = max_asize;
	*ashiftp = ashift;
}

/*
 * Close a top-level dRAID vdev.
 */
static void
vdev_draid_close(vdev_t *vd)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	for (int c = 0; c < vd->vdev_children; c++)
		vdev_close(vd->vdev_child[c]);

	if (vd->vdev_reopening || vdc == NULL)
		return;

	vdev_draid_config_destroy(vdc);
	vd->vdev_tsd = NULL;
}

/*
 * Open spare vdevs.
 */
static boolean_t
vdev_draid_open_spares(vdev_t *vd)
{
	return (vd->vdev_ops == &vdev_draid_spare_ops ||
	    vd->vdev_ops == &vdev_replacing_ops ||
	    vd->vdev_ops == &vdev_spare_ops);
}

/*
 * Open all children, excluding spares.
 */
static boolean_t
vdev_draid_open_children(vdev_t *vd)
{
	return (!vdev_draid_open_spares(vd));
}

/*
 * Open a top-level dRAID vdev.
 */
static int
vdev_draid_open(vdev_t *vd, uint64_t *asize, uint64_t *max_asize,
    uint64_t *ashift)
{
	vdev_draid_config_t *vdc;
	uint64_t nparity = vd->vdev_nparity;
	int open_errors = 0;

	if (vd->vdev_tsd != NULL) {
		/*
		 * When reopening all children must be closed and opened.
		 * The dRAID configuration itself remains valid and care
		 * is taken to avoid destroying and recreating it.
		 */
		ASSERT(vd->vdev_reopening);
		vdc = vd->vdev_tsd;
	} else {
		if (nparity > VDEV_RAIDZ_MAXPARITY ||
		    vd->vdev_children < nparity + 1) {
			vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
			return (SET_ERROR(EINVAL));
		}

		vdc = vdev_draid_config_create(vd);
		if (vdc == NULL)
			return (SET_ERROR(EINVAL));

		/*
		 * Used to generate dRAID spare names and calculate the min
		 * asize even when the vdev_draid_config_t is not available
		 * because the open fails below and the vdc is freed.
		 */
		vd->vdev_nspares = vdc->vdc_nspares;
		vd->vdev_ngroups = vdc->vdc_ngroups;
		vd->vdev_tsd = vdc;
	}

	/*
	 * First open the normal children then the distributed spares.  This
	 * ordering is important to ensure the distributed spares calculate
	 * the correct psize in the event that the dRAID vdevs were expanded.
	 */
	vdev_open_children_subset(vd, vdev_draid_open_children);
	vdev_open_children_subset(vd, vdev_draid_open_spares);

	/* Verify enough of the children are available to continue. */
	for (int c = 0; c < vd->vdev_children; c++) {
		if (vd->vdev_child[c]->vdev_open_error != 0) {
			if ((++open_errors) > nparity) {
				vd->vdev_stat.vs_aux = VDEV_AUX_NO_REPLICAS;
				return (SET_ERROR(ENXIO));
			}
		}
	}

	/*
	 * Allocatable capacity is the sum of the space on all children less
	 * the number of distributed spares rounded down to last full row
	 * and then to the last full group.
	 */
	uint64_t child_asize, child_max_asize;
	vdev_draid_calculate_asize(vd, &child_asize, &child_max_asize, ashift);

	child_asize = (child_asize / VDEV_DRAID_ROWSIZE) * VDEV_DRAID_ROWSIZE;
	child_max_asize = (child_max_asize / VDEV_DRAID_ROWSIZE) *
	    VDEV_DRAID_ROWSIZE;

	*asize = (((child_asize * vdc->vdc_ndisks) / vdc->vdc_groupsz) *
	    vdc->vdc_groupsz);
	*max_asize = (((child_max_asize * vdc->vdc_ndisks) / vdc->vdc_groupsz) *
	    vdc->vdc_groupsz);

	return (0);
}

/*
 * Return the asize of the largest block which can be reconstructed.
 */
uint64_t
vdev_draid_max_rebuildable_asize(vdev_t *vd, uint64_t max_segment)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	uint64_t psize = MIN(P2ROUNDUP(max_segment * vdc->vdc_ndata,
	    1 << vd->vdev_ashift), SPA_MAXBLOCKSIZE);

	/*
	 * When the maxpsize >> ashift does not divide evenly by the number
	 * of data drives, the remainder must be discarded. Otherwise the skip
	 * sectors will cause vdev_draid_asize_to_psize() to get a psize larger
	 * than the maximum allowed block size.
	 */
	psize >>= vd->vdev_ashift;
	psize /= vdc->vdc_ndata;
	psize *= vdc->vdc_ndata;
	psize <<= vd->vdev_ashift;

	return (vdev_draid_asize(vd, psize));
}

/*
 * Align the start of the metaslab to the group width and slightly reduce
 * its size to a multiple of the group width.  Since full stripe write are
 * required by dRAID this space is unallocatable.  Furthermore, aligning the
 * metaslab start is important for vdev initialize and TRIM which both operate
 * on metaslab boundaries which vdev_xlate() expects to be aligned.
 */
void
vdev_draid_metaslab_init(vdev_t *vd, uint64_t *ms_start, uint64_t *ms_size)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);

	uint64_t sz = vdc->vdc_groupwidth << vd->vdev_ashift;
	uint64_t astart = vdev_draid_get_astart(vd, *ms_start);
	uint64_t asize = ((*ms_size - (astart - *ms_start)) / sz) * sz;

	*ms_start = astart;
	*ms_size = asize;

	ASSERT0(*ms_start % sz);
	ASSERT0(*ms_size % sz);
}

/*
 * Returns the number of active distributed spares in the dRAID vdev tree.
 */
static int
vdev_draid_active_spares(vdev_t *vd)
{
	int spares = 0;

	if (vd->vdev_ops == &vdev_draid_spare_ops)
		return (1);

	for (int c = 0; c < vd->vdev_children; c++)
		spares += vdev_draid_active_spares(vd->vdev_child[c]);

	return (spares);
}

/*
 * Determine if any portion of the provided block resides on a child vdev
 * with a dirty DTL and therefore needs to be resilvered.
 */
static boolean_t
vdev_draid_need_resilver(vdev_t *vd, const dva_t *dva, size_t psize,
    uint64_t phys_birth)
{
	vdev_draid_config_t *vdc = vd->vdev_tsd;
	uint64_t offset = DVA_GET_OFFSET(dva);

	/*
	 * There are multiple active distributed spares, see the comment
	 * in vdev_draid_io_start() for details.
	 */
	if (vdc->vdc_nspares > 1 && vdev_draid_active_spares(vd) > 1)
		return (B_TRUE);

	if (phys_birth == TXG_UNKNOWN) {
		/*
		 * Sequential resilver.  There is no meaningful phys_birth
		 * for this block, we can only determine if block resides
		 * in a degraded group in which case it must be resilvered.
		 */
		return (vdev_draid_group_degraded(vd, offset));
	} else {
		/*
		 * Healing resilver.  TXGs not in DTL_PARTIAL are intact,
		 * as are blocks in non-degraded groups.
		 */
		if (!vdev_dtl_contains(vd, DTL_PARTIAL, phys_birth, 1))
			return (B_FALSE);

		return (vdev_draid_group_degraded(vd, offset));
	}
}

static void
vdev_draid_io_verify(zio_t *zio, raidz_map_t *rm, int col)
{
#ifdef ZFS_DEBUG
	vdev_t *vd = zio->io_vd;

	range_seg64_t logical_rs, physical_rs, remain_rs;
	logical_rs.rs_start = zio->io_offset;
	logical_rs.rs_end = logical_rs.rs_start +
	    vdev_draid_asize(zio->io_vd, zio->io_size);

	raidz_col_t *rc = &rm->rm_col[col];
	vdev_t *cvd = vd->vdev_child[rc->rc_devidx];

	vdev_xlate(cvd, &logical_rs, &physical_rs, &remain_rs);
	ASSERT(vdev_xlate_is_empty(&remain_rs));
	ASSERT3U(rc->rc_offset, ==, physical_rs.rs_start);
	ASSERT3U(rc->rc_offset, <, physical_rs.rs_end);
	ASSERT3U(rc->rc_offset + rc->rc_size, ==, physical_rs.rs_end);
#endif
}

/*
 * Start an IO operation on a dRAID vdev
 *
 * Outline:
 * - For write operations:
 *   1. Generate the parity data
 *   2. Create child zio write operations to each column's vdev, for both
 *      data and parity.  A gang ABD is allocated by vdev_draid_map_alloc()
 *      if a skip sector needs to be added to a column.
 * - For read operations:
 *   1. The vdev_draid_map_alloc() function will create a minimal raidz
 *      mapping for the read based on the zio->io_flags.  There are two
 *      possible mappings either 1) a normal read, or 2) a scrub/resilver.
 *   2. Create the zio read operations.  This will include all parity
 *      columns and skip sectors for a scrub/resilver.
 */
static void
vdev_draid_io_start(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	raidz_map_t *rm;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_ops);
	ASSERT3U(zio->io_offset, ==, vdev_draid_get_astart(vd, zio->io_offset));
	ASSERT3U(vdev_draid_offset_to_group(vd, zio->io_offset), ==,
	    vdev_draid_offset_to_group(vd, zio->io_offset + zio->io_size - 1));

	rm = vdev_draid_map_alloc(zio);

	if (zio->io_type == ZIO_TYPE_WRITE) {
		vdev_raidz_generate_parity(rm);

		/*
		 * Unlike raidz, skip sectors are zero filled and all
		 * columns must always be written.
		 */
		for (int c = 0; c < rm->rm_scols; c++) {
			raidz_col_t *rc = &rm->rm_col[c];
			vdev_t *cvd = vd->vdev_child[rc->rc_devidx];

			/*
			 * Verify physical to logical translation.
			 */
			vdev_draid_io_verify(zio, rm, c);

			zio_nowait(zio_vdev_child_io(zio, NULL, cvd,
			    rc->rc_offset, rc->rc_abd, rc->rc_size,
			    zio->io_type, zio->io_priority, 0,
			    vdev_raidz_child_done, rc));
		}

		zio_execute(zio);
		return;
	}

	ASSERT(zio->io_type == ZIO_TYPE_READ);

	/* Scrub/resilver must verify skip sectors => expanded raidz map */
	IMPLY(zio->io_flags & (ZIO_FLAG_SCRUB | ZIO_FLAG_RESILVER),
	    rm->rm_cols == rm->rm_scols);

	/* Sequential rebuild must do IO at redundancy group boundary. */
	IMPLY(zio->io_priority == ZIO_PRIORITY_REBUILD, rm->rm_nskip == 0);

	/*
	 * Iterate over the columns in reverse order so that we hit the parity
	 * last.  Any errors along the way will force us to read the parity.
	 * For scrub/resilver IOs which verify skip sectors, a gang ABD will
	 * have been allocated to store them and rc->rc_size is increased.
	 */
	for (int c = rm->rm_cols - 1; c >= 0; c--) {
		raidz_col_t *rc = &rm->rm_col[c];
		vdev_t *cvd = vd->vdev_child[rc->rc_devidx];
		vdev_t *svd;

		if (!vdev_draid_readable(cvd, rc->rc_offset)) {
			if (c >= rm->rm_firstdatacol)
				rm->rm_missingdata++;
			else
				rm->rm_missingparity++;
			rc->rc_error = SET_ERROR(ENXIO);
			rc->rc_tried = 1;
			rc->rc_skipped = 1;
			continue;
		}

		if (vdev_draid_missing(cvd, rc->rc_offset, zio->io_txg, 1)) {
			if (c >= rm->rm_firstdatacol)
				rm->rm_missingdata++;
			else
				rm->rm_missingparity++;
			rc->rc_error = SET_ERROR(ESTALE);
			rc->rc_skipped = 1;
			continue;
		}

		/*
		 * If this child is a distributed spare and we're resilvering
		 * then this offset might reside on the vdev being replaced.
		 * In which case this data must be written to the new device.
		 * Failure to do so would result in checksum errors when the
		 * old device is detached and the pool is scrubbed.
		 */
		if (zio->io_flags & ZIO_FLAG_RESILVER &&
		    (svd = vdev_draid_find_spare(cvd)) != NULL) {
			svd = vdev_draid_spare_get_child(svd, rc->rc_offset);
			if (svd && (svd->vdev_ops == &vdev_spare_ops ||
			    svd->vdev_ops == &vdev_replacing_ops)) {
				rc->rc_repair = 1;
			}
		}

		if (c >= rm->rm_firstdatacol || rm->rm_missingdata > 0 ||
		    (zio->io_flags & (ZIO_FLAG_SCRUB | ZIO_FLAG_RESILVER))) {
			zio_nowait(zio_vdev_child_io(zio, NULL, cvd,
			    rc->rc_offset, rc->rc_abd, rc->rc_size,
			    zio->io_type, zio->io_priority, 0,
			    vdev_raidz_child_done, rc));
		}
	}

	zio_execute(zio);
}

/*
 * Complete an IO operation on a dRAID vdev.  The raidz logic can be applied
 * to dRAID since the layout is fully described by the raidz_map_t.
 */
static void
vdev_draid_io_done(zio_t *zio)
{
	vdev_raidz_io_done(zio);
}

static void
vdev_draid_state_change(vdev_t *vd, int faulted, int degraded)
{
	vdev_raidz_state_change(vd, faulted, degraded);
}

static void
vdev_draid_xlate(vdev_t *cvd, const range_seg64_t *in, range_seg64_t *res)
{
	vdev_t *raidvd = cvd->vdev_parent;
	ASSERT(raidvd->vdev_ops == &vdev_draid_ops);

	vdev_draid_config_t *vdc = raidvd->vdev_tsd;
	uint64_t ashift = raidvd->vdev_top->vdev_ashift;

	/* Make sure the offsets are block-aligned */
	ASSERT0(in->rs_start % (1 << ashift));
	ASSERT0(in->rs_end % (1 << ashift));

	/*
	 * Translation requests can never span three or more slices.  Doing so
	 * could result in distributed spare space being incorrectly included
	 * in the physical range.  Therefore, vdev_xlate() limits the input
	 * size to a single group.  This is stricter than absolutely necessary
	 * but helps simplify the logic below.
	 */
	ASSERT3U(vdev_draid_offset_to_group(raidvd, in->rs_start), ==,
	    vdev_draid_offset_to_group(raidvd, in->rs_end - 1));

	/* Find the starting offset for each vdev in the group */
	uint64_t perm, groupstart;
	uint64_t start = vdev_draid_logical_to_physical(raidvd, in->rs_start,
	    &perm, &groupstart);
	uint64_t end = start;

	uint8_t *base;
	uint64_t iter, id;
	vdev_draid_get_perm(vdc, perm, &base, &iter);

	/*
	 * Check if the passed child falls within the group.  If it does
	 * update the start and end to reflect the physical range.
	 * Otherwise, leave them unmodified which will result in an empty
	 * (zero-length) physical range being returned.
	 */
	for (uint64_t i = 0; i < vdc->vdc_groupwidth; i++) {
		uint64_t c = (groupstart + i) % vdc->vdc_ndisks;

		if (c == 0 && i != 0) {
			/* the group wrapped, increment the start */
			start += VDEV_DRAID_ROWSIZE;
			end = start;
		}

		id = vdev_draid_permute_id(vdc, base, iter, c);
		if (id == cvd->vdev_id) {
			uint64_t b_size = (in->rs_end >> ashift) -
			    (in->rs_start >> ashift);
			ASSERT3U(b_size, >, 0);
			end = start + ((((b_size - 1) /
			    vdc->vdc_groupwidth) + 1) << ashift);
			break;
		}
	}
	res->rs_start = start;
	res->rs_end = end;

	ASSERT3U(res->rs_start, <=, in->rs_start);
	ASSERT3U(res->rs_end - res->rs_start, <=, in->rs_end - in->rs_start);
}

vdev_ops_t vdev_draid_ops = {
	.vdev_op_open = vdev_draid_open,
	.vdev_op_close = vdev_draid_close,
	.vdev_op_asize = vdev_draid_asize,
	.vdev_op_io_start = vdev_draid_io_start,
	.vdev_op_io_done = vdev_draid_io_done,
	.vdev_op_state_change = vdev_draid_state_change,
	.vdev_op_need_resilver = vdev_draid_need_resilver,
	.vdev_op_hold = NULL,
	.vdev_op_rele = NULL,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_draid_xlate,
	.vdev_op_type = VDEV_TYPE_DRAID,
	.vdev_op_leaf = B_FALSE,
};


/*
 * A dRAID distributed spare is a virtual leaf vdev which is included in the
 * parent dRAID configuration.  The last N columns of the dRAID permutation
 * table are used to determine on which dRAID children a specific offset
 * should be written.  These spare leaf vdevs can only be used to replace
 * faulted children in the same dRAID configuration.
 */

/*
 * Distributed spare state.  All fields are set when the distributed spare is
 * first opened and are immutable.
 */
typedef struct {
	vdev_t *vds_draid_vdev;		/* top-level parent dRAID vdev */
	uint64_t vds_spare_id;		/* spare id (0 - vdc->vdc_nspares-1) */
} vdev_draid_spare_t;

/*
 * Output a dRAID spare vdev name in to the provided buffer.
 */
char *
vdev_draid_spare_name(char *name, int len, uint64_t spare_id,
    uint64_t parity, uint64_t vdev_id)
{
	bzero(name, len);
	(void) snprintf(name, len - 1, "%s%llu-%llu-%llu",
	    VDEV_TYPE_DRAID, (u_longlong_t)parity,
	    (u_longlong_t)vdev_id, (u_longlong_t)spare_id);

	return (name);
}

/*
 * Parse dRAID configuration information out of the passed dRAID spare name.
 */
static int
vdev_draid_spare_values(const char *name, uint64_t *spare_id,
    uint64_t *parity, uint64_t *vdev_id)
{
	if (sscanf(name, VDEV_TYPE_DRAID "%llu-%llu-%llu",
	    (u_longlong_t *)parity, (u_longlong_t *)vdev_id,
	    (u_longlong_t *)spare_id) != 3) {
		return (EINVAL);
	}

	return (0);
}

/*
 * Returns the parent dRAID vdev to which the distributed spare belongs.
 * This may be safely called even when the vdev is not open.
 */
vdev_t *
vdev_draid_spare_get_parent(vdev_t *vd)
{
	uint64_t spare_id, nparity, vdev_id;
	vdev_t *rvd = vd->vdev_spa->spa_root_vdev;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_spare_ops);
	if (vdev_draid_spare_values(vd->vdev_path, &spare_id, &nparity,
	    &vdev_id) != 0) {
		return (NULL);
	}

	if (vdev_id >= rvd->vdev_children)
		return (NULL);

	return (rvd->vdev_child[vdev_id]);
}

/*
 * A dRAID space is active when it's the child of a vdev using the
 * vdev_spare_ops, vdev_replacing_ops or vdev_draid_ops.
 */
boolean_t
vdev_draid_spare_is_active(vdev_t *vd)
{
	vdev_t *pvd = vd->vdev_parent;

	if (pvd != NULL && (pvd->vdev_ops == &vdev_spare_ops ||
	    pvd->vdev_ops == &vdev_replacing_ops ||
	    pvd->vdev_ops == &vdev_draid_ops)) {
		return (B_TRUE);
	} else {
		return (B_FALSE);
	}
}

/*
 * Given a dRAID distribute spare vdev, returns the physical child vdev
 * on which the provided offset resides.  This may involve recursing through
 * multiple layers of distributed spares.  Note that offset is relative to
 * this vdev.
 */
vdev_t *
vdev_draid_spare_get_child(vdev_t *vd, uint64_t physical_offset)
{
	vdev_draid_spare_t *vds = vd->vdev_tsd;

	ASSERT3P(vd->vdev_ops, ==, &vdev_draid_spare_ops);

	/* The vdev is closed or an invalid offset was provided. */
	if (vds == NULL || physical_offset >= vd->vdev_psize -
	    (VDEV_LABEL_START_SIZE + VDEV_LABEL_END_SIZE)) {
		return (NULL);
	}

	vdev_t *tvd = vds->vds_draid_vdev;
	vdev_draid_config_t *vdc = tvd->vdev_tsd;

	ASSERT3P(tvd->vdev_ops, ==, &vdev_draid_ops);
	ASSERT3U(vds->vds_spare_id, <, vdc->vdc_nspares);

	uint8_t *base;
	uint64_t iter;
	uint64_t perm = physical_offset / vdc->vdc_devslicesz;

	vdev_draid_get_perm(vdc, perm, &base, &iter);

	uint64_t cid = vdev_draid_permute_id(vdc, base, iter,
	    (tvd->vdev_children - 1) - vds->vds_spare_id);
	vdev_t *cvd = tvd->vdev_child[cid];

	if (cvd->vdev_ops == &vdev_draid_spare_ops)
		return (vdev_draid_spare_get_child(cvd, physical_offset));

	return (cvd);
}

/*
 * Close a dRAID spare device.
 */
static void
vdev_draid_spare_close(vdev_t *vd)
{
	vdev_draid_spare_t *vds = vd->vdev_tsd;

	if (vd->vdev_reopening || vds == NULL)
		return;

	vd->vdev_tsd = NULL;
	kmem_free(vds, sizeof (vdev_draid_spare_t));
}

/*
 * Opening a dRAID spare device is done by extracting the top-level vdev id
 * and dRAID spare number from the provided vd->vdev_path identifier.  Any
 * additional information encoded in the identifier is solely used for
 * verification cross-checks and is not strictly required.
 */
static int
vdev_draid_spare_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
	uint64_t spare_id, nparity, vdev_id;
	uint64_t asize, max_asize;
	vdev_draid_config_t *vdc;
	vdev_draid_spare_t *vds;
	vdev_t *tvd, *rvd = vd->vdev_spa->spa_root_vdev;
	int error;

	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vds = vd->vdev_tsd;
		tvd = vds->vds_draid_vdev;
		vdc = tvd->vdev_tsd;
		goto skip_open;
	}

	/* Extract dRAID configuration values from the provided vdev */
	error = vdev_draid_spare_values(vd->vdev_path, &spare_id, &nparity,
	    &vdev_id);
	if (error)
		return (error);

	if (vdev_id >= rvd->vdev_children)
		return (SET_ERROR(EINVAL));

	tvd = rvd->vdev_child[vdev_id];
	vdc = tvd->vdev_tsd;

	/* Spare name references a known top-level dRAID vdev */
	if (tvd->vdev_ops != &vdev_draid_ops || vdc == NULL)
		return (SET_ERROR(EINVAL));

	/* Spare name dRAID settings agree with top-level dRAID vdev */
	if (vdc->vdc_nparity != nparity || vdc->vdc_nspares <= spare_id)
		return (SET_ERROR(EINVAL));

	vds = kmem_alloc(sizeof (vdev_draid_spare_t), KM_SLEEP);
	vds->vds_draid_vdev = tvd;
	vds->vds_spare_id = spare_id;
	vd->vdev_tsd = vds;

skip_open:
	/*
	 * Neither tvd->vdev_asize or tvd->vdev_max_asize can be used here
	 * because the caller may be vdev_draid_open() in which case the
	 * values are stale as they haven't yet been updated by vdev_open().
	 * To avoid this always recalculate the dRAID asize and max_asize.
	 */
	vdev_draid_calculate_asize(tvd, &asize, &max_asize, ashift);

	*psize = asize + VDEV_LABEL_START_SIZE + VDEV_LABEL_END_SIZE;
	*max_psize = max_asize + VDEV_LABEL_START_SIZE + VDEV_LABEL_END_SIZE;

	return (0);
}

/*
 * Completed distributed spare IO.  Store the result in the parent zio
 * as if it had performed the operation itself.  Only the first error is
 * preserved if there are multiple errors.
 */
static void
vdev_draid_spare_child_done(zio_t *zio)
{
	zio_t *pio = zio->io_private;

	if (pio->io_error == 0)
		pio->io_error = zio->io_error;
}

/*
 * Returns a valid label nvlist for the distributed spare vdev.  This is
 * used to bypass the IO pipeline to avoid the complexity of constructing
 * a complete label with valid checksum to return when read.
 */
nvlist_t *
vdev_draid_read_config_spare(vdev_t *vd)
{
	spa_t *spa = vd->vdev_spa;
	spa_aux_vdev_t *sav = &spa->spa_spares;
	uint64_t guid = vd->vdev_guid;

	nvlist_t *nv = fnvlist_alloc();
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_IS_SPARE, 1);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_CREATE_TXG, vd->vdev_crtxg);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_VERSION, spa_version(spa));
	fnvlist_add_string(nv, ZPOOL_CONFIG_POOL_NAME, spa_name(spa));
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_POOL_GUID, spa_guid(spa));
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_POOL_TXG, spa->spa_config_txg);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_TOP_GUID, vd->vdev_top->vdev_guid);
	fnvlist_add_uint64(nv, ZPOOL_CONFIG_POOL_STATE,
	    vdev_draid_spare_is_active(vd) ?
	    POOL_STATE_ACTIVE : POOL_STATE_SPARE);

	/* Set the vdev guid based on the vdev list in sav_count. */
	for (int i = 0; i < sav->sav_count; i++) {
		if (sav->sav_vdevs[i]->vdev_ops == &vdev_draid_spare_ops &&
		    strcmp(sav->sav_vdevs[i]->vdev_path, vd->vdev_path) == 0) {
			guid = sav->sav_vdevs[i]->vdev_guid;
			break;
		}
	}

	fnvlist_add_uint64(nv, ZPOOL_CONFIG_GUID, guid);

	return (nv);
}

/*
 * Handle any ioctl requested of the distributed spare.  Only flushes
 * are supported in which case all children must be flushed.
 */
static int
vdev_draid_spare_ioctl(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	int error = 0;

	if (zio->io_cmd == DKIOCFLUSHWRITECACHE) {
		for (int c = 0; c < vd->vdev_children; c++) {
			zio_nowait(zio_vdev_child_io(zio, NULL,
			    vd->vdev_child[c], zio->io_offset, zio->io_abd,
			    zio->io_size, zio->io_type, zio->io_priority, 0,
			    vdev_draid_spare_child_done, zio));
		}
	} else {
		error = SET_ERROR(ENOTSUP);
	}

	return (error);
}

/*
 * Initiate an IO to the distributed spare.  For normal IOs this entails using
 * the zio->io_offset and permutation table to calculate which child dRAID vdev
 * is responsible for the data.  Then passing along the zio to that child to
 * perform the actual IO.  The label ranges are not stored on disk and require
 * some special handling which is described below.
 */
static void
vdev_draid_spare_io_start(zio_t *zio)
{
	vdev_t *cvd = NULL, *vd = zio->io_vd;
	vdev_draid_spare_t *vds = vd->vdev_tsd;
	uint64_t offset = zio->io_offset - VDEV_LABEL_START_SIZE;

	/*
	 * If the vdev is closed, it's likely in the REMOVED or FAULTED state.
	 * Nothing to be done here but return failure.
	 */
	if (vds == NULL) {
		zio->io_error = ENXIO;
		zio_interrupt(zio);
		return;
	}

	switch (zio->io_type) {
	case ZIO_TYPE_IOCTL:
		zio->io_error = vdev_draid_spare_ioctl(zio);
		break;

	case ZIO_TYPE_WRITE:
		if (VDEV_OFFSET_IS_LABEL(vd, zio->io_offset)) {
			/*
			 * Accept probe IOs and config writers to simulate the
			 * existence of an on disk label.  vdev_label_sync(),
			 * vdev_uberblock_sync() and vdev_copy_uberblocks()
			 * skip the distributed spares.  This only leaves
			 * vdev_label_init() which is allowed to succeed to
			 * avoid adding special cases the function.
			 */
			if (zio->io_flags & ZIO_FLAG_PROBE ||
			    zio->io_flags & ZIO_FLAG_CONFIG_WRITER) {
				zio->io_error = 0;
			} else {
				zio->io_error = SET_ERROR(EIO);
			}
		} else {
			cvd = vdev_draid_spare_get_child(vd, offset);

			if (cvd == NULL || !vdev_writeable(cvd)) {
				zio->io_error = SET_ERROR(ENXIO);
			} else {
				zio_nowait(zio_vdev_child_io(zio, NULL, cvd,
				    offset, zio->io_abd, zio->io_size,
				    zio->io_type, zio->io_priority, 0,
				    vdev_draid_spare_child_done, zio));
			}
		}
		break;

	case ZIO_TYPE_READ:
		if (VDEV_OFFSET_IS_LABEL(vd, zio->io_offset)) {
			/*
			 * Accept probe IOs to simulate the existence of a
			 * label.  vdev_label_read_config() bypasses the
			 * pipeline to read the label configuration and
			 * vdev_uberblock_load() skips distributed spares
			 * when attempting to locate the best uberblock.
			 */
			if (zio->io_flags & ZIO_FLAG_PROBE) {
				zio->io_error = 0;
			} else {
				zio->io_error = SET_ERROR(EIO);
			}
		} else {
			cvd = vdev_draid_spare_get_child(vd, offset);

			if (cvd == NULL || !vdev_readable(cvd)) {
				zio->io_error = SET_ERROR(ENXIO);
			} else {
				zio_nowait(zio_vdev_child_io(zio, NULL, cvd,
				    offset, zio->io_abd, zio->io_size,
				    zio->io_type, zio->io_priority, 0,
				    vdev_draid_spare_child_done, zio));
			}
		}
		break;

	case ZIO_TYPE_TRIM:
		/* The vdev label ranges are never trimmed */
		ASSERT0(VDEV_OFFSET_IS_LABEL(vd, zio->io_offset));

		cvd = vdev_draid_spare_get_child(vd, offset);

		if (cvd == NULL || !cvd->vdev_has_trim) {
			zio->io_error = SET_ERROR(ENXIO);
		} else {
			zio_nowait(zio_vdev_child_io(zio, NULL, cvd,
			    offset, zio->io_abd, zio->io_size,
			    zio->io_type, zio->io_priority, 0,
			    vdev_draid_spare_child_done, zio));
		}
		break;

	default:
		zio->io_error = SET_ERROR(ENOTSUP);
		break;
	}

	zio_execute(zio);
}

/* ARGSUSED */
static void
vdev_draid_spare_io_done(zio_t *zio)
{
}

vdev_ops_t vdev_draid_spare_ops = {
	.vdev_op_open = vdev_draid_spare_open,
	.vdev_op_close = vdev_draid_spare_close,
	.vdev_op_asize = vdev_default_asize,
	.vdev_op_io_start = vdev_draid_spare_io_start,
	.vdev_op_io_done = vdev_draid_spare_io_done,
	.vdev_op_state_change = NULL,
	.vdev_op_need_resilver = NULL,
	.vdev_op_hold = NULL,
	.vdev_op_rele = NULL,
	.vdev_op_remap = NULL,
	.vdev_op_xlate = vdev_default_xlate,
	.vdev_op_type = VDEV_TYPE_DRAID_SPARE,
	.vdev_op_leaf = B_TRUE,
};
