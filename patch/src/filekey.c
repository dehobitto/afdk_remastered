/* filekey.c - "Read beyond end of file" when a project is opened through File > Open (e.g. Lab1.AFP), while the same
   file opens fine from the command line.  A few fields of AFDK files are scrambled with a key held in one global
   variable (*(*(0x4e4db8)) + 0x10).  It is set when the library is read at start-up, and project files are written and
   read with it.  The File > Open path (FUN_004dadac) first validates the chosen file with FUN_004da770, which as a side
   effect replaces that key by one derived from the project file itself - wrong for files saved in another session -
   so the project is then decoded with the wrong key.  The command-line path skips that check and works.
   Fix: the check in the File > Open path keeps the key as it was.
   Also: the key mixes in a machine value (FUN_0047f0a0: Windows folder date, free disk space, Windows version) that is
   computed several times; when it moves in between (anything written to the disk) reading fails too.  It cancels out
   of the format, so it is made constant.  Included from feat.c. */

#define A_KEY_HOLDER  0x4e4db8
#define A_OPEN_CHECK  0x4dadfb         /* call FUN_004da770 inside FUN_004dadac (File > Open) */
#define F_FILE_CHECK  0x4da770

unsigned afdk_check_fn = F_FILE_CHECK;
unsigned afdk_saved_key;

unsigned *afdk_file_key(void)
{
    unsigned *holder = *(unsigned **)A_KEY_HOLDER;
    return holder && *holder ? (unsigned *)(*holder + 0x10) : NULL;
}

/* register convention in and out (EAX, EDX, ECX -> AL): save the key, run the check, put the key back */
__asm__(
    ".text\n"
    ".globl _afdk_open_check\n"
    "_afdk_open_check:\n"
    "  pushl %eax\n  pushl %edx\n  pushl %ecx\n"
    "  call _afdk_file_key\n"
    "  testl %eax, %eax\n  jz 1f\n  movl (%eax), %eax\n1:\n"
    "  movl %eax, _afdk_saved_key\n"
    "  popl %ecx\n  popl %edx\n  popl %eax\n"
    "  call *_afdk_check_fn\n"
    "  pushl %eax\n"
    "  call _afdk_file_key\n"
    "  testl %eax, %eax\n  jz 2f\n  movl _afdk_saved_key, %edx\n  movl %edx, (%eax)\n2:\n"
    "  popl %eax\n"
    "  ret\n");
void afdk_open_check(void);

static void fix_file_keys(void)
{
    unsigned char *p = (unsigned char *)A_OPEN_CHECK, *k = (unsigned char *)0x47f0a0;
    DWORD old;
    if (p[0] == 0xE8 && (unsigned)(p + 5 + *(int *)(p + 1)) == F_FILE_CHECK && VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        *(int *)(p + 1) = (int)((unsigned char *)afdk_open_check - (p + 5));
        VirtualProtect(p, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
    }
    if (k[0] != 0xB8 && VirtualProtect(k, 6, PAGE_EXECUTE_READWRITE, &old)) {           /* constant machine value */
        k[0] = 0xB8; *(DWORD *)(k + 1) = 0x5A17C0DEu; k[5] = 0xC3;                         /* mov eax, const ; ret */
        VirtualProtect(k, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), k, 6);
    }
}
