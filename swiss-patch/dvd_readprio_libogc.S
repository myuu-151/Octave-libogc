/*
 * dvd_readprio_libogc.S  --  Swiss patch stub (WIP, for the Swiss FORK)
 *
 * Redirects libogc's synchronous DVD_ReadPrio() to Swiss's SD-disc reader so an
 * on-disc Octave/libogc ISO reads its assets under Swiss. This does NOT go into
 * Octave -- it goes into a Swiss fork, installed when the patcher recognizes
 * libogc's DVD_ReadPrio by fingerprint {60,12,11,2,6,4} (Length,Loads,Stores,
 * FCalls,Branch,Moves per make_pattern @ patcher.c:499).
 *
 * libogc DVD_ReadPrio ABI (verified by disassembling the call sites in our
 * binary -- NOT the SysV default, note the register gap):
 *     r3 = dvdcmdblk*        (ignored)
 *     r4 = void*  buffer     (destination, cached 0x8xxxxxxx effective addr)
 *     r5 = s32    length     (bytes)
 *     r7 = s32    offset_hi  (high 32 of the s64 disc offset; 0 for our ISOs)
 *     r8 = s32    offset_lo  (low 32 = the byte offset into the disc)
 *     r9 = s32    prio       (ignored)
 *   returns r3 = bytes read; libogc callers only test (< 0), so we return the
 *   length on success and -1 on short read.
 *
 * Swiss side we call:
 *     int frag_read_complete(int file, void *buffer, u32 length, u32 offset);
 *   synchronous, fragmentation-aware, returns bytes read. The mounted ISO is
 *   *VAR_CURRENT_DISC. We read into the buffer's UNCACHED alias (0xC0000000),
 *   the same trick perform_read() uses (OSPhysicalToUncached), then invalidate
 *   the cached range so the game's later cached reads refetch from memory.
 *
 * OPEN INTEGRATION POINTS (resolve when wiring into the Swiss fork):
 *   - VAR_CURRENT_DISC : reserved-area address (reservedarea.h). Referenced
 *     here as an extern symbol; confirm it resolves at link time in the patch.
 *   - frag_read_complete : linked from frag.c in the same patch image.
 *   - Cache invalidate : using an inline dcbi loop to stay self-contained.
 *   - This must be built as a patch blob (bin2s -> _bin.h) and installed as the
 *     FuncPattern.Patch for the DVD_ReadPrio signature, overwriting the found
 *     function's entry with a branch here (see how sd_v2_bin etc. are installed).
 */

	.section .text
	.globl DVDReadPrio_libogc
DVDReadPrio_libogc:
	mflr	r0
	stwu	r1, -32(r1)
	stw	r0, 36(r1)
	stw	r31, 28(r1)
	stw	r30, 24(r1)

	mr	r31, r4			# r31 = buffer (cached, for the invalidate pass)
	mr	r30, r5			# r30 = length (saved for return compare)

	# --- read into the UNCACHED alias of the buffer (avoids stale cache) ---
	rlwinm	r4, r4, 0, 2, 31	# r4 = physical  (clear top 2 bits of 0x8xxxxxxx)
	oris	r4, r4, 0xC000		# r4 = uncached  (0xCxxxxxxx)
	# r5 already = length
	mr	r6, r8			# r6 = offset (low 32 of the s64)
	lis	r3, VAR_CURRENT_DISC@ha
	lwz	r3, VAR_CURRENT_DISC@l(r3)	# r3 = *VAR_CURRENT_DISC (file handle)
	bl	frag_read_complete	# r3 = bytes read

	# --- invalidate the cached buffer range so the CPU refetches ---
	# r31 = buffer, r30 = length; walk in 32-byte cache lines
	mr	r9, r30
	addi	r9, r9, 31
	srwi	r9, r9, 5		# r9 = number of 32-byte lines
	mtctr	r9
	mr	r10, r31
1:	dcbi	0, r10
	addi	r10, r10, 32
	bdnz	1b
	sync

	# --- return: length on success, -1 on short read ---
	cmpw	r3, r30
	bge	2f
	li	r3, -1
2:
	lwz	r0, 36(r1)
	lwz	r31, 28(r1)
	lwz	r30, 24(r1)
	addi	r1, r1, 32
	mtlr	r0
	blr
