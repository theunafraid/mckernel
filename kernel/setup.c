#include <types.h>
#include <kmsg.h>
#include <aal/cpu.h>
#include <aal/mm.h>
#include <aal/debug.h>

extern struct aal_kmsg_buf kmsg_buf;

extern void arch_init(void);
extern void kmsg_init(void);
extern void mem_init(void);
extern void ikc_master_init(void);
extern void arch_ready(void);
extern void mc_ikc_init(void);

int main(void)
{
	kmsg_init();

	kputs("MCK started.\n");

	arch_init();

	mem_init();

	ikc_master_init();

	mc_ikc_init();

	arch_ready();
	cpu_enable_interrupt();

	kputs("MCK/AAL booted.\n");

	while (1) {
		cpu_halt();
	}
	return 0;
}
