#ifndef SYSCALLS_H
#define SYSCALLS_H

/* New syscalls involved by MultiLane */
#define __NR_uintr_register_irq_handler   471
// #define __NR_uintr_register_uinv_handler  472
#define __NR_uintr_reset_upid_page        473
#define __NR_uintr_free_upid_page         474
// #define __NR_get_uintr_status             476
#define __NR_uintr_set_sn                 477
#define __NR_uintr_init_uitt              478
// #define __NR_uintr_set_pir_bit            479
#define __NR_uintr_wait                   480
#define __NR_uintr_register_queue_task    481
#define __NR_uintr_unregister_queue_task  482
#define __NR_uintr_kernel_handler_enable  483
#define __NR_uintr_kernel_handler_disable 484
#define __NR_uintr_register_timer_int     485
#define __NR_uintr_unregister_timer_int   486
// #define __NR_read_timer_msr               488
#define __NR_register_lapic_ctl           489
#define __NR_unregister_lapic_ctl         490

#endif /* SYSCALLS_H */
