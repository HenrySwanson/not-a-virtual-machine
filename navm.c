/*
 * CS 11, C track, lab 8
 *
 * FILE: navm.c
 *       Because abusing the rules of C (and common sense) is fun!
 *       Inspired by: http://stackoverflow.com/a/5602143
 *
 *       I decided to make the virtual machine a little less virtual. Instead,
 *       this behaves somewhat like a just-in-time compiler.
 *
 *       On a 32-bit computer, compile normally. If on a 64-bit computer,
 *       it must be compiled in 32-bit mode, using gcc's -m32 option.
 *
 *       This code is hilariously unsafe. Don't use anywhere that requires
 *       any degree of portability.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define MAX_INSTS 65536
#define NREGS 16

/* The largest number of x86 instructions a CS11 instruction can expand to */
#define MAX_SCALE 12

/*
 * The CS11 instruction set.  Each instruction fits into a single byte.
 *
 * Glossary:
 * --------
 *   TOS: "top of the stack".
 *   Sn:  contents of stack position n (S1 == TOS)
 *   <n>: integer
 *   <i>: instruction
 *   <r>: register
 *
 * Notes:
 * -----
 *
 * 1) All operations that take their operands from the stack
 *    (i.e. ADD, SUB, MUL, DIV) pop these operands from the stack
 *    before putting the result back to the TOS.  STORE, JZ, JNZ,
 *    and PRINT also pop the TOS after they do their work.
 *
 * 2) Many operations take additional arguments from the instruction
 *    stream: PUSH, LOAD, STORE, JMP, JZ, JNZ.  These arguments
 *    are NOT found on the stack but are read in from the bytecode.
 *    They have the following lengths:
 *
 *    a) integers:     4 bytes (signed)
 *    b) instructions: 2 bytes (unsigned)
 *    c) registers:    1 byte (unsigned)
 *
 * 3) LOAD operations DO NOT erase the contents of a register.
 *
 */

/* --------------------- usage: ----------------------------------- */
#define NOP     0x00  /* NOP: do nothing.                           */
#define PUSH    0x01  /* PUSH <n>: push <n> to TOS.                 */
#define POP     0x02  /* POP: pop TOS.                              */
#define LOAD    0x03  /* LOAD <r>: load register <r> to TOS.        */
#define STORE   0x04  /* STORE <r>: store TOS to register <r>
                         and pop the TOS.                           */
#define JMP     0x05  /* JMP <i>: go to instruction <i>.            */
#define JZ      0x06  /* JZ <i>: if TOS is zero,
                         pop TOS and go to instruction <i>;
                         else just pop TOS.                         */
#define JNZ     0x07  /* JNZ <i>: if TOS is nonzero,
                         pop TOS and go to instruction <i>;
                         else just pop TOS.                         */
#define ADD     0x08  /* ADD: S2 + S1 -> TOS                        */
#define SUB     0x09  /* SUB: S2 - S1 -> TOS                        */
#define MUL     0x0a  /* MUL: S2 * S1 -> TOS                        */
#define DIV     0x0b  /* DIV: S2 / S1 -> TOS                        */
#define PRINT   0x0c  /* PRINT: print TOS to stdout and pop TOS.    */
#define STOP    0x0d  /* STOP: halt the program.                    */

/* Convenience */
typedef unsigned char byte;

/* Used when implementing the PRINT instruction */
char* format_str = "%d\n";

/* Used to construct a stack of jump instructions */
typedef struct _node
{
    int real_pos; /* The location of the address to change (real code) */
    short fake_addr; /* The address of the fake instruction to jump to */
    struct _node* next;
} node;

/* Puts a new node on top of a pre-existing stack (including NULL) */
node* prepend_to_list(int i, int j, node* n)
{
    node* m = malloc(sizeof(node));
    m->real_pos = i;
    m->fake_addr = j;
    m->next = n;
    return m;
}

/* Prints a usage statement and exits */
void usage(char *progname)
{
    fprintf(stderr, "usage: %s filename\n", progname);
    exit(1);
}

/* Checks for failure to allocate memory. Cleaner than inlining it. */
void check_for_null(void * ptr)
{
    if (ptr == NULL)
    {
        fprintf(stderr, "Error: memory allocation failed! "
                        "Terminating program.\n");
        exit(1);
    }
}

/* Reads the given file, and returns an array of the instructions */
byte* load_file(const char* filename, int* out_len)
{
    FILE *fp;
    int nread;
    byte *code;
    byte *ptr;

    /* Open the file containing the bytecode. */
    fp = fopen(filename, "r");
    if (fp == NULL)
    {
        fprintf(stderr, "Error opening file %s; aborting.\n", filename);
        exit(1);
    }

    /* Get file size */
    fseek(fp, 0L, SEEK_END);
    *out_len = ftell(fp);
    fseek(fp, 0L, SEEK_SET);

    if(*out_len > MAX_INSTS)
    {
        fprintf(stderr, "Error: file larger than %d bytes!\n", MAX_INSTS);
        exit(1);
    }

    code = calloc(sizeof(byte), *out_len);
    check_for_null(code);

    ptr = code;
    do
    {
        /*
         * Reads a byte, and stores it where ptr points.
         * 'fread' returns the number of bytes read, or 0 if EOF is hit.
         */
        nread = fread(ptr, 1, 1, fp);
        ptr++;
    }
    while (nread > 0);  

    fclose(fp);
    return code;
}

/*
 * Takes the opcodes for the CS 11 virtual machine and converts them into
 * x86 machine code. Exceedingly long because the alternative is shoving
 * around a whole bunch of parameters that don't need to be passed around.
 *
 * Arguments:
 *     fake_ptr - the virtual instructions, as an array of bytes
 *     fake_len - the number of bytes for fake_ptr
 *     real_ptr - the address to which the real opcodes should be written
 *
 * Returns: the number of bytes written to real_ptr.
 *
 * Conversion notes: memcpy is used to write sequences of bytes into memory.
 *     The sequences are expressed as strings for compactness, using the
 *     escape sequence \x## (hexadecimal character). The fact that these are
 *     null-terminated does not matter, because we specify the number of
 *     bytes to copy.
 *     
 *     Because we will not know the destination of a jump in
 *     advance, we have to write the address after the first scan completes.
 *     We initially set them to 0xCABBA6E5 (cabbages), because in a hexdump,
 *     that is recognizable and uncommon, so we can tell if we forgot to set
 *     the addresses.
 *
 *     Implementing the print instruction was pretty much the stupidest thing
 *     I have ever decided to do. This is pretty much impossible to document,
 *     but I'll try. I attempt to call printf("%d\n", x), where x is the
 *     element on top of the stack. Since the convention is that arguments
 *     go onto the stack in reverse order, I only have to push "%d\n", which
 *     I do by pushing format_str. Then, I compute the offset of the printf
 *     function pointer from the next instruction after call, because nearly
 *     every way of calling functions requires relative addresses. After the
 *     call, I pop both arguments off the stack, which cleans it up, and
 *     popping x off the top of the stack, as desired.
 *
 * The conversion between virtual and real opcodes is below:
 *
 *     NOP:
 *         90                - nop
 *
 *     PUSH:
 *         68 ## ## ## ##    - push <n>
 *
 *     POP:
 *         5A                - pop %edx
 *
 *     LOAD:
 *         FF 75 ##          - push <n>[ebp]
 *
 *     STORE:
 *         8F 45 ##          - pop <n>[ebp]
 *
 *     JMP:
 *         E9 ## ## ## ##    - jmp <n>
 *
 *     JZ:
 *         59                - pop %ecx
 *         85 C9             - test %ecx, %ecx
 *         0F 84 ## ## ## ## - jz <n>
 *
 *     JNZ:
 *         59                - pop %ecx
 *         85 C9             - test %ecx, %ecx
 *         0F 85 ## ## ## ## - jnz <n>
 *
 *     ADD:
 *         5A                - pop %edx
 *         59                - pop %ecx
 *         01 D1             - add %ecx, %edx
 *         51                - push %ecx
 *
 *     SUB:
 *         5A                - pop %edx
 *         59                - pop %ecx
 *         29 D1             - sub %ecx, %edx
 *         51                - push %ecx
 *
 *     MUL:
 *         5A                - pop %edx
 *         59                - pop %ecx
 *         0F AF CA          - imul %ecx, %edx
 *         51                - push %ecx
 *
 *     DIV:                    
 *         59                - pop %ecx
 *         58                - pop %eax
 *         31 D2             - xor %edx, %edx
 *         F7 F9             - idiv %ecx
 *         50                - push %eax
 *
 *     PRINT:
 *         68 ## ## ## ##    - push <n>
 *         E8 ## ## ## ##    - call <a>
 *         5A                - pop %edx
 *         5A                - pop %edx
 *
 *     STOP:
 *         E9 ## ## ## ##    - jmp <n>
 */
int convert_opcodes(byte* fake_ptr, byte* real_ptr, int fake_len)
{
    int i = 0;
    int j = 0;
    int* addr_table;
    node* jump_locations = NULL;

    /*
     * For each address with a CS11 instruction, contains the corresponding
     * address in the x86 machine code.
     * Since STOP has to jump to one after the end of these instructions,
     * we have to extend our array of addresses by one.
     */
    addr_table = calloc(sizeof(int), fake_len + 1);
    check_for_null(addr_table);

    /*
     * Scan through the CS11 machine code, and fill in real_ptr with 
     * x86 machine code. However, jumps are not correctly filled in here.
     */
    while(i < fake_len)
    {
        /* These two are declared here because you cannot declare in a case */
        short jmp_addr;    /* Used in JMP, JZ, and JNZ */
        int printf_offset; /* Used in PRINT */

        addr_table[i] = j;

        switch(fake_ptr[i])
        {
        case NOP:
            real_ptr[j] = 0x90;
            i++;
            j++;
            break;

        case PUSH:
            real_ptr[j] = 0x68;
            memcpy(real_ptr + j + 1, fake_ptr + i + 1, 4);
            i += 5;
            j += 5;
            break;

        case POP:
            real_ptr[j] = 0x5A;
            i++;
            j++;
            break;

        case LOAD:
            memcpy(real_ptr + j, "\xFF\x75", 2);
            real_ptr[j + 2] = -4 * (fake_ptr[i + 1] + 1);
            i += 2;
            j += 3;
            break;

        case STORE:
            memcpy(real_ptr + j, "\x8F\x45", 2);
            real_ptr[j + 2] = -4 * (fake_ptr[i + 1] + 1);
            i += 2;
            j += 3;
            break;

        case JMP:
            memcpy(real_ptr + j, "\xE9\xCA\xBB\xA6\xE5", 5);
            jmp_addr = fake_ptr[i + 1] + (fake_ptr[i + 2] << 8);
            jump_locations = prepend_to_list(j + 1, jmp_addr, jump_locations);
            i += 3;
            j += 5;
            break;

        case JZ:
            memcpy(real_ptr + j, "\x59\x85\xC9\x0F\x84\xCA\xBB\xA6\xE5", 9);
            jmp_addr = fake_ptr[i + 1] + (fake_ptr[i + 2] << 8);
            jump_locations = prepend_to_list(j + 5, jmp_addr, jump_locations);
            i += 3;
            j += 9;
            break;

        case JNZ:
            memcpy(real_ptr + j, "\x59\x85\xC9\x0F\x85\xCA\xBB\xA6\xE5", 9);
            jmp_addr = fake_ptr[i + 1] + (fake_ptr[i + 2] << 8);
            jump_locations = prepend_to_list(j + 5, jmp_addr, jump_locations);
            i += 3;
            j += 9;
            break;

        case ADD:
            memcpy(real_ptr + j, "\x5A\x59\x01\xD1\x51", 5);
            i++;
            j += 5;
            break;

        case SUB:
            memcpy(real_ptr + j, "\x5A\x59\x29\xD1\x51", 5);
            i++;
            j += 5;
            break;

        case MUL:
            memcpy(real_ptr + j, "\x5A\x59\x0F\xAF\xCA\x51", 6);
            i++;
            j += 6;
            break;

        case DIV:
            memcpy(real_ptr + j, "\x59\x58\x31\xD2\xF7\xF9\x50", 7);
            i++;
            j += 7;
            break;

        case PRINT:
            real_ptr[j] = 0x68;
            memcpy(real_ptr + j + 1, &format_str, 4);
            printf_offset = (void *)(&printf) - (void *)(real_ptr + j + 10);
            real_ptr[j + 5] = 0xE8;
            memcpy(real_ptr + j + 6, &printf_offset, 4);
            memcpy(real_ptr + j + 10, "\x5A\x5A", 2);
            i++;
            j += 12;
            break;

        case STOP:
            memcpy(real_ptr + j, "\xE9\xCA\xBB\xA6\xE5", 5);
            jump_locations = prepend_to_list(j + 1, fake_len, jump_locations);
            i++;
            j += 5;
            break;

        default:
            fprintf(stderr, "execute_program: invalid instruction: %x\n",
                    fake_ptr[i]);
            fprintf(stderr, "\taborting program!\n");
            exit(1);
        }
    }

    /* Put in the very last address */
    addr_table[fake_len] = j;

    /* Fill in the jumps */
    while(jump_locations != NULL)
    {
        node* temp = jump_locations;

        /* We add 4 because an address is 4 bytes long */
        int addr_offset = addr_table[temp->fake_addr] - (temp->real_pos + 4);
        memcpy(real_ptr + temp->real_pos, &addr_offset, 4);

        jump_locations = temp->next;
        free(temp);
    }

    return j;
}

/*
 * Takes a sequence of CS11 opcodes, and returns a pointer to a sequence
 * of x86 opcodes which do the same operations. This can be treated
 * exactly like a function pointer.
 *
 * Conversion notes: there are only 8 registers, and two are taken
 * by %esp and %ebp. So we push 16 zeros onto the stack to simulate
 * them. The registers themselves are used for emulation.
 */
byte* perform_conversion(byte* fake, int fake_len)
{
    int i;
    byte* real;
    byte* fake_ptr;
    byte* real_ptr;

    /* Grabs a chunk of executable memory */
    real = mmap(0, MAX_INSTS * MAX_SCALE,
                   PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check_for_null(real);

    fake_ptr = fake;
    real_ptr = real;

    /* Set up ESP, EBP and 'registers' */
    *(real_ptr++) = 0x55; /* push ebp */
    *(real_ptr++) = 0x89; /* move ebp, esp */    
    *(real_ptr++) = 0xE5; /* ^ */
    for(i = 0; i < NREGS; i++)
    {
        *(real_ptr++) = 0x6A; /* push 0 */
        *(real_ptr++) = 0x00; /* ^ */
    }

    real_ptr += convert_opcodes(fake_ptr, real_ptr, fake_len);

    /* Pop 'registers' and restore ESP and EBP */
    for(i = 0; i < NREGS; i++)
    {
        *(real_ptr++) = 0x58; /* pop %eax */
    }
    *(real_ptr++) = 0xC9; /* leave */
    *(real_ptr++) = 0xC3; /* ret */

    return real;
}


int main(int argc, char **argv)
{
    byte* fake_code;
    int fake_len;
    byte* real_code;

    if (argc != 2)
    {
        usage(argv[0]);
        exit(1);
    }

    fake_code = load_file(argv[1], &fake_len);
    real_code = perform_conversion(fake_code, fake_len);

    /* Calls function, because C is full of black casting magic */
    ((int(*)()) real_code)(); /* Seriously, how is this legal C ? */

    free(fake_code);
    munmap(real_code, MAX_INSTS * MAX_SCALE);

    return 0;
}

