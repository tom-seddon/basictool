// TODO: CHECK ALL FILES TO MAKE SURE "LOCAL" FNS ARE STATIC
#include "emulation.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "data.h"
#include "lib6502.h"
#include "utils.h"

// TODO: I am quite inconsistent about mpu->foo vs just using these structure
// directly. I should be consistent, but when choosing how to be consistent,
// perhaps favour the shortest code?
// TODO: SOME/ALL OF THESE SHOULD MAYBE BE STATIC?
M6502_Registers mpu_registers;
M6502_Memory mpu_memory;
M6502_Callbacks mpu_callbacks;
M6502 *mpu;

// M6502_run() never returns, so we use this jmp_buf to return control when a
// task has finished executing on the emulated CPU.
jmp_buf mpu_env;

#define ROM_SIZE (16 * 1024)

int vdu_variables[257];

// TODO: This probably needs expanding etc, I'm hacking right now
// TODO: Should probably not check this via assert(), it *shouldn't* go wrong
// if there are no program bugs, but it is quite likely some strange situations
// can occur if given invalid user input and ABE or BASIC does something I am not expecting in response.
enum {
    SFTODOIDLE, // SFTODO: NOT "IDLE", RENAME THIS
    osword_input_line_pending,
    osrdch_pending,
} state = SFTODOIDLE; // SFTODO: 'state' IS TOO SHORT A NAME

static void mpu_write_u16(uint16_t address, uint16_t data) {
    mpu_memory[address    ] = data & 0xff;
    mpu_memory[address + 1] = (data >> 8) & 0xff;
}

uint16_t mpu_read_u16(uint16_t address) {
    return (mpu_memory[address + 1] << 8) | mpu_memory[address];
}

static void mpu_clear_carry(M6502 *mpu) {
    mpu->registers->p &= ~(1<<0);
}

static void mpu_dump(void) {
    char buffer[64];
    M6502_dump(mpu, buffer);
    fprintf(stderr, "6502 state: %s\n", buffer);
}

// TODO: COPY AND PASTE OF enter_basic()
static uint16_t enter_basic2(void) {
    mpu_registers.a = 1; // language entry special value in A
    mpu_registers.x = 0;
    mpu_registers.y = 0;

    const uint16_t code_address = 0x900;
    uint8_t *p = &mpu_memory[code_address];
    *p++ = 0xa2; *p++ = 12;                // LDX #12 TODO: MAGIC CONSTANT
    *p++ = 0x86; *p++ = 0xf4;              // STX &F4
    *p++ = 0x8e; *p++ = 0x30; *p++ = 0xfe; // STX &FE30
    *p++ = 0x4c; *p++ = 0x00; *p++ = 0x80; // JMP &8000 (language entry)

    return code_address;
}

NORETURN static void callback_abort(const char *type, uint16_t address, uint8_t data) {
    die("Error: Unexpected %s at address %04x, data %02x", type, address, data);
}

NORETURN static int callback_abort_read(M6502 *mpu, uint16_t address, uint8_t data) {
    // externalise() hasn't been called by lib6502 at this point so we can't
    // dump the registers.
    callback_abort("read", address, data);
}

NORETURN static int callback_abort_write(M6502 *mpu, uint16_t address, uint8_t data) {
    // externalise() hasn't been called by lib6502 at this point so we can't
    // dump the registers.
    callback_abort("write", address, data);
}

NORETURN static int callback_abort_call(M6502 *mpu, uint16_t address, uint8_t data) {
    mpu_dump();
    callback_abort("call", address, data);
}

static int callback_return_via_rts(M6502 *mpu) {
    uint8_t low  = mpu->memory[0x101 + mpu->registers->s];
    uint8_t high = mpu->memory[0x102 + mpu->registers->s];
    mpu->registers->s += 2;
    uint16_t address = (high << 8) | low;
    address += 1;
    //fprintf(stderr, "SFTODOXXX %04x\n", address);
    return address;
}

static int callback_osrdch(M6502 *mpu, uint16_t address, uint8_t data) {
    //fprintf(stderr, "SFTODOSSA\n");
    state = osrdch_pending;
    longjmp(mpu_env, 1);
}

static int callback_oswrch(M6502 *mpu, uint16_t address, uint8_t data) {
    int c = mpu->registers->a;
    pending_output_insert(c);
    return callback_return_via_rts(mpu);
}

static int callback_osnewl(M6502 *mpu, uint16_t address, uint8_t data) {
    pending_output_insert(0xa);
    pending_output_insert(0xd);
    return callback_return_via_rts(mpu);
}

static int callback_osasci(M6502 *mpu, uint16_t address, uint8_t data) {
    int c = mpu->registers->a;
    if (c == 0xd) {
        return callback_osnewl(mpu, address, data);
    } else {
        return callback_oswrch(mpu, address, data);
    }
}

static int callback_osbyte_return_x(M6502 *mpu, uint8_t x) {
    mpu->registers->x = x;
    return callback_return_via_rts(mpu);
}

static int callback_osbyte_return_u16(M6502 *mpu, uint16_t value) {
    mpu->registers->x = value & 0xff;
    mpu->registers->y = (value >> 8) & 0xff;
    return callback_return_via_rts(mpu);
}

static int callback_osbyte_read_vdu_variable(M6502 *mpu) {
    int i = mpu->registers->x;
    if (vdu_variables[i] == -1) {
        mpu_dump();
        die("Error: Unsupported VDU variable %d read", i);
    }
    if (vdu_variables[i + 1] == -1) {
        mpu_dump();
        die("Error: Unsupported VDU variable %d read", i + 1);
    }
    mpu->registers->x = vdu_variables[i];
    mpu->registers->y = vdu_variables[i + 1];
    return callback_return_via_rts(mpu);
}

// TODO: Not here (this has to follow std prototype), but get rid of pointless mpu argument on some functions?
static int callback_osbyte(M6502 *mpu, uint16_t address, uint8_t data) {
    switch (mpu->registers->a) {
        case 0x03: // select output device
            return callback_return_via_rts(mpu); // treat as no-op
        case 0x0f: // flush buffers
            return callback_return_via_rts(mpu); // treat as no-op
        case 0x7c: // clear Escape condition
            return callback_return_via_rts(mpu); // treat as no-op
        case 0x7e: // acknowledge Escape condition
            return callback_osbyte_return_x(mpu, 0); // no Escape condition pending
        case 0x83: // read OSHWM
            return callback_osbyte_return_u16(mpu, page);
        case 0x84: // read HIMEM
            return callback_osbyte_return_u16(mpu, himem);
        case 0x86: // read text cursor position
            return callback_osbyte_return_u16(mpu, 0); // TODO: MAGIC CONST, HACK
        case 0x8a: // place character into buffer
            // TODO: We probably shouldn't be treating this is a no-op but let's hack
            return callback_return_via_rts(mpu); // treat as no-op
        case 0xa0:
            return callback_osbyte_read_vdu_variable(mpu);
        default:
            mpu_dump();
            die("Error: Unsupported OSBYTE");
    }
}

static int callback_oscli(M6502 *mpu, uint16_t address, uint8_t data) {
    uint16_t yx = (mpu_registers.y << 8) | mpu_registers.x;
    mpu_memory[0xf2] = mpu_registers.x;
    mpu_memory[0xf3] = mpu_registers.y;
    //fprintf(stderr, "SFTODOXCC %c%c%c\n", mpu_memory[yx], mpu_memory[yx+1], mpu_memory[yx+2]);

    mpu_registers.a = 4; // unrecognised * command
    mpu_registers.x = 1; // current ROM bank TODO: MAGIC HACK, WE KNOW ABE IS BANKS 0 AND 1 AND THEY ARE THE ONLY BANKS WE NEED TO PASS SERVICE CALLS TO - IDEALLy WE'D RUN OVER ALL ROMS, THO BASIC HAS NO SERVICE ENTRY OF COURSE
    mpu_registers.y = 0; // command tail offset

    // TODO: It would be possible to write all this in assembler and have it
    // pre-built into a "mini OS" which we load at 0xc000 or 0xf000 or something.
    // However, we'd then need an assembler as part of the build process. We
    // could hand-assemble it like this, but do it once on startup, but then
    // we'd have to write more code (e.g. this "*" skipped loop) in hand-
    // assembled code, which would be painful.

    // Skip leading "*" on the command; this is essential to have it recognised
    // properly (as that's what the real OS does).
    while (mpu_memory[yx + mpu_registers.y] == '*') {
        ++mpu_registers.y;
        check(mpu_registers.y <= 255, "Too many * on OSCLI"); // unlikely!
    }

    // This isn't case-insensitive and doesn't recognise abbreviations, but
    // in practice it's good enough.
    if (memcmp(&mpu_memory[yx + mpu_registers.y], "BASIC", 5) == 0) {
        //fprintf(stderr, "SFTODO BASIC!\n");
        return enter_basic2();
    }

    const uint16_t code_address = 0xb00; // TODO: HACK - OTHER BITS OF HACKERY CAN OVERWRITE THIS WHILE WE'RE PART WAY THROUGH EXECUTING *BUTIL IF WE USE 0x900 - NO, THAT DOESN'T HELP, MAYBE THIS WOULD BE FINE, BUT LET'S LEAVE IT AT B00 FOR NOW
    uint8_t *p = &mpu_memory[code_address];
                                           // .loop
    *p++ = 0x86; *p++ = 0xf4;              // STX &F4
    *p++ = 0x8e; *p++ = 0x30; *p++ = 0xfe; // STX &FE30
    *p++ = 0x20; *p++ = 0x03; *p++ = 0x80; // JSR &8003 (service entry)
    *p++ = 0xa6; *p++ = 0xf4;              // LDX &F4
    *p++ = 0xca;                           // DEX
    *p++ = 0x10; *p++ = 256 - 13;          // BPL loop
    *p++ = 0xc9; *p++ = 0;                 // CMP #0
    *p++ = 0xd0; *p++ = 1;                 // BNE skip_rts
    *p++ = 0x60;                           // RTS
                                           // .skip_rts
    *p++ = 0x00;                           // BRK
    *p++ = 0xfe;                           // error code
    strcpy(p, "Bad command");              // error string and terminator
    
    //fprintf(stderr, "SFTODO999\n");
    return code_address;
}

static int callback_osword_input_line(M6502 *mpu) {
    //fprintf(stderr, "SFTODOXA2\n");
    state = osword_input_line_pending;
    longjmp(mpu_env, 1);
}

static int callback_osword_read_io_memory(M6502 *mpu) {
    // So we don't bypass any lib6502 callbacks, we do this access via a
    // dynamically generated code stub.
    const uint16_t code_address = 0x900;
    uint8_t *p = &mpu_memory[code_address];
    uint16_t yx = (mpu->registers->y << 8) | mpu->registers->x;
    uint16_t source = mpu_read_u16(yx);
    uint16_t dest = yx + 4;
    *p++ = 0xad; *p++ = source & 0xff; *p++ = (source >> 8) & 0xff; // LDA source
    *p++ = 0x8d; *p++ = dest & 0xff; *p++ = (dest >> 8) & 0xff;     // STA dest
    *p++ = 0x60;                                                    // RTS
    return code_address;
}

static int callback_osword(M6502 *mpu, uint16_t address, uint8_t data) {
    switch (mpu->registers->a) {
        case 0x00: // input line
            return callback_osword_input_line(mpu);
        case 0x05: // read I/O processor memory
            return callback_osword_read_io_memory(mpu);
        default:
            mpu_dump();
            die("Error: Unsupported OSWORD");
    }
}

static int callback_read_escape_flag(M6502 *mpu, uint16_t address, uint8_t data) {
    return 0; // Escape flag not set
}

static int callback_romsel_write(M6502 *mpu, uint16_t address, uint8_t data) {
    switch (data) {
        // TODO: The bank numbers should be named constants
        case 0:
            memcpy(&mpu_memory[0x8000], rom_editor_a, ROM_SIZE);
            break;
        case 1:
            memcpy(&mpu_memory[0x8000], rom_editor_b, ROM_SIZE);
            break;
        case 12: // same bank as on Master 128, but not really important
            memcpy(&mpu_memory[0x8000], rom_basic, ROM_SIZE);
            break;
        default:
            check(false, "Invalid ROM bank selected");
            break;
    }
    return 0; // return value ignored
}

static int callback_irq(M6502 *mpu, uint16_t address, uint8_t data) {
    // The only possible cause of an interrupt on our emulated machine is a BRK
    // instruction.
    // TODO: Copy and paste of code from callback_return_via_rts() - not quite
    uint8_t low  = mpu->memory[0x102 + mpu->registers->s];
    uint8_t high = mpu->memory[0x103 + mpu->registers->s];
    mpu->registers->s += 2; // TODO not necessary, we won't return
    uint16_t error_string_ptr = (high << 8) | low;
    uint16_t error_num_address = error_string_ptr - 1;
    fprintf(stderr, "Error: ");
    for (uint8_t c; (c = mpu->memory[error_string_ptr]) != '\0'; ++error_string_ptr) {
        fputc(c, stderr);
    }
    uint8_t error_num = mpu->memory[error_num_address];
    // TODO: We will need an ability to include a pseudo-line number if we're tokenising a BASIC program - but maybe not just here, maybe on other errors too (e.g. in die()?)
    fprintf(stderr, " (%d)\n", error_num);
    exit(EXIT_FAILURE);
}

static void callback_poll(M6502 *mpu) {
}

static void set_abort_callback(uint16_t address) {
    M6502_setCallback(mpu, read,  address, callback_abort_read);
    // TODO: Get rid of write callback permanently?
    //M6502_setCallback(mpu, write, address, callback_abort_write);
}

void init(void) { // TODO: RENAME
    mpu = check_alloc(M6502_new(&mpu_registers, mpu_memory, &mpu_callbacks));
    M6502_reset(mpu);
    
    // Install handlers to abort on read or write of anywhere in OS workspace;
    // this will catch anything we haven't explicitly implemented. Addresses
    // 0x90-0xaf are used, but they don't contain OS state we need to emulate
    // so this loop excludes them.
    for (uint16_t address = 0xb0; address < 0x100; ++address) {
        switch (address) {
            case 0xa8:
            case 0xa9:
            case 0xf2:
            case 0xf3:
            case 0xf4:
                // Supported as far as necessary, don't install a handler.
                break;

            default:
                set_abort_callback(address);
                break;
        }
    }

    // Trap access to unimplemented OS vectors.
    for (uint16_t address = 0x200; address < 0x236; ++address) {
        switch (address) {
            case 0x20e:
            case 0x20f:
                // Supported as far as necessary, don't install a handler.
                break;
            default:
                set_abort_callback(address);
                break;
        }
    }
#if 0 // SFTODO!? PROB GET RID OF THIS NOW I REALLY AM RUNNING BASIC
    // TODO: If things aren't working, perhaps tighten this up - I'm assuming
    // pages 5/6/7 don't contain interesting BASIC housekeeping data for ABE.
    for (uint16_t address = 0x400; address < 0x500; ++address) {
        set_abort_callback(address);
    }
#endif

    // Install handlers for OS entry points, using a default for unimplemented
    // ones.
    for (uint16_t address = 0xc000; address != 0; ++address) {
        M6502_setCallback(mpu, call, address, callback_abort_call);
    }
    M6502_setCallback(mpu, call, 0xffe0, callback_osrdch);
    M6502_setCallback(mpu, call, 0xffe3, callback_osasci);
    M6502_setCallback(mpu, call, 0xffe7, callback_osnewl);
    M6502_setCallback(mpu, call, 0xffee, callback_oswrch);
    M6502_setCallback(mpu, call, 0xfff1, callback_osword);
    M6502_setCallback(mpu, call, 0xfff4, callback_osbyte);
    M6502_setCallback(mpu, call, 0xfff7, callback_oscli);

    // Install fake OS vectors. Because of the way our implementation works,
    // these vectors actually point to the official entry points.
    mpu_write_u16(0x20e, 0xffee);

    // Since we don't have an actual Escape handler, just ensure any read from
    // &ff always returns 0.
    M6502_setCallback(mpu, read, 0xff, callback_read_escape_flag);

    // Install handler for hardware ROM paging emulation.
    M6502_setCallback(mpu, write, 0xfe30, callback_romsel_write);

    // Install interrupt handler so we can catch BRK.
    M6502_setVector(mpu, IRQ, 0xf000); // SFTODO: Magic constant
    M6502_setCallback(mpu, call, 0xf000, callback_irq);

    // Set up VDU variables.
    for (int i = 0; i < 256; ++i) {
        vdu_variables[i] = -1;
    }
    vdu_variables[0x55] = 7; // screen mode
    vdu_variables[0x56] = 4; // memory map type: 1K mode
}

// TODO: MOVE
// TODO: RENAME
void execute_osrdch(const char *s) {
    assert(strlen(s) == 1);
    assert(state == osrdch_pending);
    char c = s[0];
    mpu->registers->a = c;
    mpu_clear_carry(mpu); // no error
    // TODO: Following code fragment may be common to OSWORD 0 and can be factored out
    mpu->registers->pc = callback_return_via_rts(mpu);
    //fprintf(stderr, "SFTODOZX022 %d\n", c);
    // SFTODO: WE SHOULD PROBABLY ALWAYS SET STATE TO SOMETHING WHEN WE DO M6502_RUN
    if (setjmp(mpu_env) == 0) {
        //fprintf(stderr, "SFTODORUN\n");
        state = SFTODOIDLE;
        M6502_run(mpu, callback_poll); // never returns
    }
    //fprintf(stderr, "SFTODOZXA22\n");
} 

// TODO: MOVE
void execute_input_line(const char *line) {
    assert(state == osword_input_line_pending);
    //fprintf(stderr, "SFTODOXA\n");
    // TODO: We must respect the maximum line length
    uint16_t yx = (mpu->registers->y << 8) | mpu->registers->x;
    uint16_t buffer = mpu_read_u16(yx);
    uint8_t buffer_size = mpu_memory[yx + 2];
    size_t pending_length = strlen(line);
    check(pending_length < buffer_size, "Line too long"); // TODO: PROPER ERROR ETC - BUT WE DO WANT TO TREAT THIS AS AN ERROR, NOT TRUNCATE - WE MAY ULTIMATELY WANT TO BE GIVING A LINE NUMBER FROM INPUT IF WE'RE TOKENISING BASIC VIA THIS
    memcpy(&mpu_memory[buffer], line, pending_length);

    // OSWORD 0 would echo the typed characters and move to a new line, so do
    // the same with our pending output.
    for (int i = 0; i < pending_length; ++i) {
        pending_output_insert(line[i]);
    }
    pending_output_insert(0xa); pending_output_insert(0xd);

    mpu_memory[buffer + pending_length] = 0xd;
    mpu->registers->y = pending_length; // TODO HACK
    mpu_clear_carry(mpu); // input not terminated by Escape
    mpu->registers->pc = callback_return_via_rts(mpu);
    //fprintf(stderr, "SFTODOZX0\n");
    if (setjmp(mpu_env) == 0) {
        state = SFTODOIDLE;
        M6502_run(mpu, callback_poll); // never returns
    }
    //fprintf(stderr, "SFTODOZXA\n");
}

void enter_basic(void) {
    mpu_registers.a = 1; // language entry special value in A
    mpu_registers.x = 0;
    mpu_registers.y = 0;

    const uint16_t code_address = 0x900;
    uint8_t *p = &mpu_memory[code_address];
    *p++ = 0xa2; *p++ = 12;                // LDX #12 TODO: MAGIC CONSTANT
    *p++ = 0x86; *p++ = 0xf4;              // STX &F4
    *p++ = 0x8e; *p++ = 0x30; *p++ = 0xfe; // STX &FE30
    *p++ = 0x4c; *p++ = 0x00; *p++ = 0x80; // JMP &8000 (language entry)

    mpu_registers.s  = 0xff;
    mpu_registers.pc = code_address;
    if (setjmp(mpu_env) == 0) {
        M6502_run(mpu, callback_poll); // never returns
    }
}

