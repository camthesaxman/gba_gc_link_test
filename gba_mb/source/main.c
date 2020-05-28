#include <gba_console.h>
#include <gba_video.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_sio.h>
#include <gba_input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOY_WRITE (1 << 1)
#define JOY_READ  (1 << 2)

// REG_HS_CTRL = JOYCNT
// REG_JSTAT = JOYSTAT

static void dump_regs(void)
{
    printf("RCNT  %04X     JOYCNT %04X\n"
          "JOYRE %08X JOYTR  %08x\n"
          "JSTAT %08x\n",
          (u16)REG_RCNT, (u16)REG_HS_CTRL,
          (unsigned int)REG_JOYRE, (unsigned int)REG_JOYTR,
          (unsigned int)REG_JSTAT);
}

int main(int argc, char **argv)
{
    u32 sendVal = 0;

	irqInit();
	irqEnable(IRQ_VBLANK);
    consoleDemoInit();

    puts("JOYBUS TEST");

    while (1)
    {
        scanKeys();
        u16 pressed = keysDown();

        if (REG_HS_CTRL & JOY_WRITE)
        {
            printf("<<< recv 0x%08X\n", (unsigned int)REG_JOYRE);
            REG_HS_CTRL |= JOY_WRITE;
        }

        if (REG_HS_CTRL & JOY_READ)
            REG_HS_CTRL |= JOY_READ;

        if (pressed & KEY_START)
        {
            puts("Registers:");
            dump_regs();
        }

        if (pressed & KEY_A)  // Send message
        {
            REG_JOYTR = sendVal;
            printf(">>> send 0x%08X\n", (unsigned int)sendVal);
        }

        if (pressed & KEY_UP)
            sendVal++;
        if (pressed & KEY_DOWN)
            sendVal--;

        VBlankIntrWait();
    }
}
