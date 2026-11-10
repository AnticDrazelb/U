/* xbox-core: QEMU's USB trace points, compiled out */
#include <stdint.h>
#include <stdbool.h>
#define trace_event_get_state_backends(id) 0
static inline void trace_usb_packet_state_change(int bus, const char *port, int ep, void *p, const char *o, const char *n) {}
static inline void trace_usb_packet_state_fault(int bus, const char *port, int ep, void *p, const char *o, const char *n) {}
static inline void trace_usb_port_claim(int bus, const char *port) {}
static inline void trace_usb_port_attach(int bus, const char *port, const char *devspeed, const char *portspeed) {}
static inline void trace_usb_port_detach(int bus, const char *port) {}
static inline void trace_usb_port_release(int bus, const char *port) {}
static inline void trace_usb_ohci_exit(const char *s) {}
static inline void trace_usb_ohci_iso_td_read_failed(uint32_t addr) {}
static inline void trace_usb_ohci_iso_td_head(uint32_t head, uint32_t tail, uint32_t flags, uint32_t bp, uint32_t next, uint32_t be, uint32_t framenum, uint32_t startframe, uint32_t framecount, int rel_frame_num) {}
static inline void trace_usb_ohci_iso_td_head_offset(uint32_t o0, uint32_t o1, uint32_t o2, uint32_t o3, uint32_t o4, uint32_t o5, uint32_t o6, uint32_t o7) {}
static inline void trace_usb_ohci_iso_td_relative_frame_number_neg(int rel) {}
static inline void trace_usb_ohci_iso_td_relative_frame_number_big(int rel, int count) {}
static inline void trace_usb_ohci_iso_td_bad_direction(int dir) {}
static inline void trace_usb_ohci_iso_td_bad_bp_be(uint32_t bp, uint32_t be) {}
static inline void trace_usb_ohci_iso_td_bad_cc_not_accessed(uint32_t start, uint32_t next) {}
static inline void trace_usb_ohci_iso_td_bad_cc_overrun(uint32_t start, uint32_t next) {}
static inline void trace_usb_ohci_iso_td_so(uint32_t so, uint32_t eo, uint32_t s, uint32_t e, const char *str, ssize_t len, int ret) {}
static inline void trace_usb_ohci_iso_td_data_overrun(int ret, ssize_t len) {}
static inline void trace_usb_ohci_iso_td_data_underrun(int ret) {}
static inline void trace_usb_ohci_iso_td_nak(int ret) {}
static inline void trace_usb_ohci_iso_td_bad_response(int ret) {}
static inline void trace_usb_ohci_td_bad_pid(const char *s, uint32_t edf, uint32_t tdf) {}
static inline void trace_usb_ohci_td_bad_buf(uint32_t cbp, uint32_t be) {}
static inline void trace_usb_ohci_port_attach(int index) {}
static inline void trace_usb_ohci_port_detach(int index) {}
static inline void trace_usb_ohci_port_wakeup(int index) {}
static inline void trace_usb_ohci_port_suspend(int index) {}
static inline void trace_usb_ohci_port_reset(int index) {}
static inline void trace_usb_ohci_remote_wakeup(const char *s) {}
static inline void trace_usb_ohci_reset(const char *s) {}
static inline void trace_usb_ohci_start(const char *s) {}
static inline void trace_usb_ohci_resume(const char *s) {}
static inline void trace_usb_ohci_stop(const char *s) {}
static inline void trace_usb_ohci_set_ctl(const char *s, uint32_t new_state) {}
static inline void trace_usb_ohci_td_underrun(void) {}
static inline void trace_usb_ohci_td_dev_error(void) {}
static inline void trace_usb_ohci_td_nak(void) {}
static inline void trace_usb_ohci_td_stall(void) {}
static inline void trace_usb_ohci_td_babble(void) {}
static inline void trace_usb_ohci_td_bad_device_response(int rc) {}
static inline void trace_usb_ohci_td_read_error(uint32_t addr) {}
static inline void trace_usb_ohci_td_bad_direction(int dir) {}
static inline void trace_usb_ohci_td_skip_async(void) {}
static inline void trace_usb_ohci_td_pkt_hdr(uint32_t addr, int64_t pktlen, int64_t len, const char *s, int flag_r, uint32_t cbp, uint32_t be) {}
static inline void trace_usb_ohci_td_pkt_short(const char *dir, const char *buf) {}
static inline void trace_usb_ohci_td_pkt_full(const char *dir, const char *buf) {}
static inline void trace_usb_ohci_td_too_many_pending(int ep) {}
static inline void trace_usb_ohci_td_packet_status(int status) {}
static inline void trace_usb_ohci_ed_read_error(uint32_t addr) {}
static inline void trace_usb_ohci_ed_pkt(uint32_t cur, int h, int c, uint32_t head, uint32_t tail, uint32_t next) {}
static inline void trace_usb_ohci_ed_pkt_flags(uint32_t fa, uint32_t en, uint32_t d, int s, int k, int f, uint32_t mps) {}
static inline void trace_usb_ohci_hcca_read_error(uint32_t addr) {}
static inline void trace_usb_ohci_mem_read(uint32_t size, const char *name, uint32_t addr, uint32_t offs, uint32_t val) {}
static inline void trace_usb_ohci_mem_port_read(uint32_t size, const char *name, uint32_t port, uint32_t addr, uint32_t offs, uint32_t val) {}
static inline void trace_usb_ohci_mem_read_unaligned(uint32_t addr) {}
static inline void trace_usb_ohci_mem_read_bad_offset(uint32_t addr) {}
static inline void trace_usb_ohci_mem_write(uint32_t size, const char *name, uint32_t addr, uint32_t offs, uint32_t val) {}
static inline void trace_usb_ohci_mem_port_write(uint32_t size, const char *name, uint32_t port, uint32_t addr, uint32_t offs, uint32_t val) {}
static inline void trace_usb_ohci_mem_write_unaligned(uint32_t addr) {}
static inline void trace_usb_ohci_mem_write_bad_offset(uint32_t addr) {}
static inline void trace_usb_ohci_process_lists(uint32_t head, uint32_t cur) {}
static inline void trace_usb_ohci_set_frame_interval(const char *name, uint16_t fi_x, uint16_t fi_u) {}
static inline void trace_usb_ohci_hub_power_up(void) {}
static inline void trace_usb_ohci_hub_power_down(void) {}
static inline void trace_usb_ohci_init_time(int64_t frametime, int64_t bittime) {}
static inline void trace_usb_ohci_die(void) {}
static inline void trace_usb_ohci_async_complete(void) {}
static inline void trace_usb_ehci_reset(void) {}
static inline void trace_usb_ehci_unrealize(void) {}
static inline void trace_usb_ehci_opreg_read(uint32_t addr, const char *str, uint32_t val) {}
static inline void trace_usb_ehci_opreg_write(uint32_t addr, const char *str, uint32_t val) {}
static inline void trace_usb_ehci_opreg_change(uint32_t addr, const char *str, uint32_t new, uint32_t old) {}
static inline void trace_usb_ehci_portsc_read(uint32_t addr, uint32_t port, uint32_t val) {}
static inline void trace_usb_ehci_portsc_write(uint32_t addr, uint32_t port, uint32_t val) {}
static inline void trace_usb_ehci_portsc_change(uint32_t addr, uint32_t port, uint32_t new, uint32_t old) {}
static inline void trace_usb_ehci_usbsts(const char *sts, int state) {}
static inline void trace_usb_ehci_state(const char *schedule, const char *state) {}
static inline void trace_usb_ehci_qh_ptrs(void *q, uint32_t addr, uint32_t nxt, uint32_t c_qtd, uint32_t n_qtd, uint32_t a_qtd) {}
static inline void trace_usb_ehci_qh_fields(uint32_t addr, int rl, int mplen, int eps, int ep, int devaddr) {}
static inline void trace_usb_ehci_qh_bits(uint32_t addr, int c, int h, int dtc, int i) {}
static inline void trace_usb_ehci_qtd_ptrs(void *q, uint32_t addr, uint32_t nxt, uint32_t altnext) {}
static inline void trace_usb_ehci_qtd_fields(uint32_t addr, int tbytes, int cpage, int cerr, int pid) {}
static inline void trace_usb_ehci_qtd_bits(uint32_t addr, int ioc, int active, int halt, int babble, int xacterr) {}
static inline void trace_usb_ehci_itd(uint32_t addr, uint32_t nxt, uint32_t mplen, uint32_t mult, uint32_t ep, uint32_t devaddr) {}
static inline void trace_usb_ehci_sitd(uint32_t addr, uint32_t nxt, uint32_t active) {}
static inline void trace_usb_ehci_port_attach(uint32_t port, const char *owner, const char *device) {}
static inline void trace_usb_ehci_port_detach(uint32_t port, const char *owner) {}
static inline void trace_usb_ehci_port_reset(uint32_t port, int enable) {}
static inline void trace_usb_ehci_port_suspend(uint32_t port) {}
static inline void trace_usb_ehci_port_wakeup(uint32_t port) {}
static inline void trace_usb_ehci_port_resume(uint32_t port) {}
static inline void trace_usb_ehci_queue_action(void *q, const char *action) {}
static inline void trace_usb_ehci_packet_action(void *q, void *p, const char *action) {}
static inline void trace_usb_ehci_irq(uint32_t level, uint32_t frindex, uint32_t sts, uint32_t mask) {}
static inline void trace_usb_ehci_guest_bug(const char *reason) {}
static inline void trace_usb_ehci_doorbell_ring(void) {}
static inline void trace_usb_ehci_doorbell_ack(void) {}
static inline void trace_usb_ehci_dma_error(void) {}
static inline void trace_usb_uhci_reset(void) {}
static inline void trace_usb_uhci_exit(void) {}
static inline void trace_usb_uhci_schedule_start(void) {}
static inline void trace_usb_uhci_schedule_stop(void) {}
static inline void trace_usb_uhci_frame_start(uint32_t num) {}
static inline void trace_usb_uhci_frame_stop_bandwidth(void) {}
static inline void trace_usb_uhci_frame_loop_stop_idle(void) {}
static inline void trace_usb_uhci_frame_loop_continue(void) {}
static inline void trace_usb_uhci_mmio_readw(uint32_t addr, uint32_t val) {}
static inline void trace_usb_uhci_mmio_writew(uint32_t addr, uint32_t val) {}
static inline void trace_usb_uhci_queue_add(uint32_t token) {}
static inline void trace_usb_uhci_queue_del(uint32_t token, const char *reason) {}
static inline void trace_usb_uhci_packet_add(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_link_async(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_unlink_async(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_cancel(uint32_t token, uint32_t addr, int done) {}
static inline void trace_usb_uhci_packet_complete_success(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_complete_shortxfer(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_complete_stall(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_complete_babble(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_complete_error(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_packet_del(uint32_t token, uint32_t addr) {}
static inline void trace_usb_uhci_qh_load(uint32_t qh) {}
static inline void trace_usb_uhci_td_load(uint32_t qh, uint32_t td, uint32_t ctrl, uint32_t token) {}
static inline void trace_usb_uhci_td_queue(uint32_t td, uint32_t ctrl, uint32_t token) {}
static inline void trace_usb_uhci_td_nextqh(uint32_t qh, uint32_t td) {}
static inline void trace_usb_uhci_td_async(uint32_t qh, uint32_t td) {}
static inline void trace_usb_uhci_td_complete(uint32_t qh, uint32_t td) {}
static inline void trace_usb_xhci_reset(void) {}
static inline void trace_usb_xhci_exit(void) {}
static inline void trace_usb_xhci_run(void) {}
static inline void trace_usb_xhci_stop(void) {}
static inline void trace_usb_xhci_cap_read(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_oper_read(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_port_read(uint32_t port, uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_runtime_read(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_doorbell_read(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_oper_write(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_port_write(uint32_t port, uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_runtime_write(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_doorbell_write(uint32_t off, uint32_t val) {}
static inline void trace_usb_xhci_irq_intx(uint32_t level) {}
static inline void trace_usb_xhci_irq_msi(uint32_t nr) {}
static inline void trace_usb_xhci_irq_msix(uint32_t nr) {}
static inline void trace_usb_xhci_irq_msix_use(uint32_t nr) {}
static inline void trace_usb_xhci_irq_msix_unuse(uint32_t nr) {}
static inline void trace_usb_xhci_queue_event(uint32_t vector, uint32_t idx, const char *trb, const char *evt, uint64_t param, uint32_t status, uint32_t control) {}
static inline void trace_usb_xhci_fetch_trb(uint64_t addr, const char *name, uint64_t param, uint32_t status, uint32_t control) {}
static inline void trace_usb_xhci_port_reset(uint32_t port, bool warm) {}
static inline void trace_usb_xhci_port_link(uint32_t port, uint32_t pls) {}
static inline void trace_usb_xhci_port_notify(uint32_t port, uint32_t pls) {}
static inline void trace_usb_xhci_slot_enable(uint32_t slotid) {}
static inline void trace_usb_xhci_slot_disable(uint32_t slotid) {}
static inline void trace_usb_xhci_slot_address(uint32_t slotid, const char *port) {}
static inline void trace_usb_xhci_slot_configure(uint32_t slotid) {}
static inline void trace_usb_xhci_slot_evaluate(uint32_t slotid) {}
static inline void trace_usb_xhci_slot_reset(uint32_t slotid) {}
static inline void trace_usb_xhci_ep_enable(uint32_t slotid, uint32_t epid) {}
static inline void trace_usb_xhci_ep_disable(uint32_t slotid, uint32_t epid) {}
static inline void trace_usb_xhci_ep_set_dequeue(uint32_t slotid, uint32_t epid, uint32_t streamid, uint64_t param) {}
static inline void trace_usb_xhci_ep_kick(uint32_t slotid, uint32_t epid, uint32_t streamid) {}
static inline void trace_usb_xhci_ep_stop(uint32_t slotid, uint32_t epid) {}
static inline void trace_usb_xhci_ep_reset(uint32_t slotid, uint32_t epid) {}
static inline void trace_usb_xhci_ep_state(uint32_t slotid, uint32_t epid, const char *os, const char *ns) {}
static inline void trace_usb_xhci_xfer_start(void *xfer, uint32_t slotid, uint32_t epid, uint32_t streamid) {}
static inline void trace_usb_xhci_xfer_async(void *xfer) {}
static inline void trace_usb_xhci_xfer_nak(void *xfer) {}
static inline void trace_usb_xhci_xfer_retry(void *xfer) {}
static inline void trace_usb_xhci_xfer_success(void *xfer, uint32_t bytes) {}
static inline void trace_usb_xhci_xfer_error(void *xfer, uint32_t ret) {}
static inline void trace_usb_xhci_unimplemented(const char *item, int nr) {}
static inline void trace_usb_xhci_enforced_limit(const char *item) {}
static inline void trace_usb_dwc2_update_irq(uint32_t level) {}
static inline void trace_usb_dwc2_raise_global_irq(uint32_t intr) {}
static inline void trace_usb_dwc2_lower_global_irq(uint32_t intr) {}
static inline void trace_usb_dwc2_raise_host_irq(uint32_t intr) {}
static inline void trace_usb_dwc2_lower_host_irq(uint32_t intr) {}
static inline void trace_usb_dwc2_sof(int64_t next) {}
static inline void trace_usb_dwc2_bus_start(void) {}
static inline void trace_usb_dwc2_bus_stop(void) {}
static inline void trace_usb_dwc2_find_device(uint8_t addr) {}
static inline void trace_usb_dwc2_port_disabled(uint32_t pnum) {}
static inline void trace_usb_dwc2_device_found(uint32_t pnum) {}
static inline void trace_usb_dwc2_device_not_found(void) {}
static inline void trace_usb_dwc2_handle_packet(uint32_t chan, void *dev, void *pkt, uint32_t ep, const char *type, const char *dir, uint32_t mps, uint32_t len, uint32_t pcnt) {}
static inline void trace_usb_dwc2_memory_read(uint32_t addr, uint32_t len) {}
static inline void trace_usb_dwc2_packet_status(const char *status, uint32_t len) {}
static inline void trace_usb_dwc2_packet_error(const char *status) {}
static inline void trace_usb_dwc2_async_packet(void *pkt, uint32_t chan, void *dev, uint32_t ep, const char *dir, uint32_t len) {}
static inline void trace_usb_dwc2_memory_write(uint32_t addr, uint32_t len) {}
static inline void trace_usb_dwc2_packet_done(const char *status, uint32_t actual, uint32_t len, uint32_t pcnt) {}
static inline void trace_usb_dwc2_packet_next(const char *status, uint32_t len, uint32_t pcnt) {}
static inline void trace_usb_dwc2_attach(void *port) {}
static inline void trace_usb_dwc2_attach_speed(const char *speed) {}
static inline void trace_usb_dwc2_detach(void *port) {}
static inline void trace_usb_dwc2_child_detach(void *port, void *child) {}
static inline void trace_usb_dwc2_wakeup(void *port) {}
static inline void trace_usb_dwc2_async_packet_complete(void *port, void *pkt, uint32_t chan, void *dev, uint32_t ep, const char *dir, uint32_t len) {}
static inline void trace_usb_dwc2_work_bh(void) {}
static inline void trace_usb_dwc2_work_bh_service(uint32_t first, uint32_t current, void *dev, uint32_t ep) {}
static inline void trace_usb_dwc2_work_bh_next(uint32_t chan) {}
static inline void trace_usb_dwc2_enable_chan(uint32_t chan, void *dev, void *pkt, uint32_t ep) {}
static inline void trace_usb_dwc2_glbreg_read(uint64_t addr, const char *reg, uint32_t val) {}
static inline void trace_usb_dwc2_glbreg_write(uint64_t addr, const char *reg, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_fszreg_read(uint64_t addr, uint32_t val) {}
static inline void trace_usb_dwc2_fszreg_write(uint64_t addr, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_hreg0_read(uint64_t addr, const char *reg, uint32_t val) {}
static inline void trace_usb_dwc2_hreg0_write(uint64_t addr, const char *reg, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_hreg1_read(uint64_t addr, const char *reg, uint64_t chan, uint32_t val) {}
static inline void trace_usb_dwc2_hreg1_write(uint64_t addr, const char *reg, uint64_t chan, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_pcgreg_read(uint64_t addr, const char *reg, uint32_t val) {}
static inline void trace_usb_dwc2_pcgreg_write(uint64_t addr, const char *reg, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_hreg2_read(uint64_t addr, uint64_t fifo, uint32_t val) {}
static inline void trace_usb_dwc2_hreg2_write(uint64_t addr, uint64_t fifo, uint64_t val, uint32_t old, uint64_t result) {}
static inline void trace_usb_dwc2_hreg0_action(const char *s) {}
static inline void trace_usb_dwc2_wakeup_endpoint(void *ep, uint32_t stream) {}
static inline void trace_usb_dwc2_work_timer(void) {}
static inline void trace_usb_dwc2_reset_enter(void) {}
static inline void trace_usb_dwc2_reset_hold(void) {}
static inline void trace_usb_dwc2_reset_exit(void) {}
static inline void trace_usb_desc_device(int addr, int len, int ret) {}
static inline void trace_usb_desc_device_qualifier(int addr, int len, int ret) {}
static inline void trace_usb_desc_config(int addr, int index, int len, int ret) {}
static inline void trace_usb_desc_other_speed_config(int addr, int index, int len, int ret) {}
static inline void trace_usb_desc_string(int addr, int index, int len, int ret) {}
static inline void trace_usb_desc_bos(int addr, int len, int ret) {}
static inline void trace_usb_desc_msos(int addr, int index, int len, int ret) {}
static inline void trace_usb_set_addr(int addr) {}
static inline void trace_usb_set_config(int addr, int config, int ret) {}
static inline void trace_usb_set_interface(int addr, int iface, int alt, int ret) {}
static inline void trace_usb_clear_device_feature(int addr, int feature, int ret) {}
static inline void trace_usb_set_device_feature(int addr, int feature, int ret) {}
static inline void trace_usb_hub_reset(int addr) {}
static inline void trace_usb_hub_control(int addr, int request, int value, int index, int length) {}
static inline void trace_usb_hub_get_port_status(int addr, int nr, int status, int changed) {}
static inline void trace_usb_hub_set_port_feature(int addr, int nr, const char *f) {}
static inline void trace_usb_hub_clear_port_feature(int addr, int nr, const char *f) {}
static inline void trace_usb_hub_attach(int addr, int nr) {}
static inline void trace_usb_hub_detach(int addr, int nr) {}
static inline void trace_usb_hub_status_report(int addr, int status) {}
static inline void trace_usb_msd_reset(void) {}
static inline void trace_usb_msd_maxlun(unsigned maxlun) {}
static inline void trace_usb_msd_send_status(unsigned status, unsigned tag, size_t size) {}
static inline void trace_usb_msd_data_in(unsigned packet, unsigned remaining, unsigned total) {}
static inline void trace_usb_msd_data_out(unsigned packet, unsigned remaining) {}
static inline void trace_usb_msd_packet_async(void) {}
static inline void trace_usb_msd_packet_complete(void) {}
static inline void trace_usb_msd_cmd_submit(unsigned lun, unsigned tag, unsigned flags, unsigned len, unsigned data_len) {}
static inline void trace_usb_msd_cmd_complete(unsigned status, unsigned tag) {}
static inline void trace_usb_msd_cmd_cancel(unsigned tag) {}
static inline void trace_usb_msd_fatal_error(void) {}
static inline void trace_usb_uas_reset(int addr) {}
static inline void trace_usb_uas_command(int addr, uint16_t tag, int lun, uint32_t lun64_1, uint32_t lun64_2) {}
static inline void trace_usb_uas_response(int addr, uint16_t tag, uint8_t code) {}
static inline void trace_usb_uas_sense(int addr, uint16_t tag, uint8_t status) {}
static inline void trace_usb_uas_read_ready(int addr, uint16_t tag) {}
static inline void trace_usb_uas_write_ready(int addr, uint16_t tag) {}
static inline void trace_usb_uas_xfer_data(int addr, uint16_t tag, uint32_t copy, uint32_t uoff, uint32_t usize, uint32_t soff, uint32_t ssize) {}
static inline void trace_usb_uas_scsi_data(int addr, uint16_t tag, uint32_t bytes) {}
static inline void trace_usb_uas_scsi_complete(int addr, uint16_t tag, uint32_t status, uint32_t resid) {}
static inline void trace_usb_uas_tmf_abort_task(int addr, uint16_t tag, uint16_t task_tag) {}
static inline void trace_usb_uas_tmf_logical_unit_reset(int addr, uint16_t tag, int lun) {}
static inline void trace_usb_uas_tmf_unsupported(int addr, uint16_t tag, uint32_t function) {}
static inline void trace_usb_mtp_reset(int addr) {}
static inline void trace_usb_mtp_command(int dev, uint16_t code, uint32_t trans, uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {}
static inline void trace_usb_mtp_success(int dev, uint32_t trans, uint32_t arg0, uint32_t arg1) {}
static inline void trace_usb_mtp_error(int dev, uint16_t code, uint32_t trans, uint32_t arg0, uint32_t arg1) {}
static inline void trace_usb_mtp_data_in(int dev, uint32_t trans, uint32_t len) {}
static inline void trace_usb_mtp_xfer(int dev, uint32_t ep, uint32_t dlen, uint32_t plen) {}
static inline void trace_usb_mtp_nak(int dev, uint32_t ep) {}
static inline void trace_usb_mtp_stall(int dev, const char *reason) {}
static inline void trace_usb_mtp_op_get_device_info(int dev) {}
static inline void trace_usb_mtp_op_open_session(int dev) {}
static inline void trace_usb_mtp_op_close_session(int dev) {}
static inline void trace_usb_mtp_op_get_storage_ids(int dev) {}
static inline void trace_usb_mtp_op_get_storage_info(int dev) {}
static inline void trace_usb_mtp_op_get_num_objects(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_op_get_object_handles(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_op_get_object_info(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_op_get_object(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_op_get_partial_object(int dev, uint32_t handle, const char *path, uint32_t offset, uint32_t length) {}
static inline void trace_usb_mtp_op_unknown(int dev, uint32_t code) {}
static inline void trace_usb_mtp_object_alloc(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_object_free(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_add_child(int dev, uint32_t handle, const char *path) {}
static inline void trace_usb_mtp_file_monitor_event(int dev, const char *path, const char *s) {}
static inline void trace_usb_host_open_started(int bus, int addr) {}
static inline void trace_usb_host_open_hostfd(int hostfd) {}
static inline void trace_usb_host_open_success(int bus, int addr) {}
static inline void trace_usb_host_open_failure(int bus, int addr) {}
static inline void trace_usb_host_close(int bus, int addr) {}
static inline void trace_usb_host_attach_kernel(int bus, int addr, int interface) {}
static inline void trace_usb_host_detach_kernel(int bus, int addr, int interface) {}
static inline void trace_usb_host_set_address(int bus, int addr, int config) {}
static inline void trace_usb_host_set_config(int bus, int addr, int config) {}
static inline void trace_usb_host_set_interface(int bus, int addr, int interface, int alt) {}
static inline void trace_usb_host_claim_interface(int bus, int addr, int config, int interface) {}
static inline void trace_usb_host_release_interface(int bus, int addr, int interface) {}
static inline void trace_usb_host_req_control(int bus, int addr, void *p, int req, int value, int index) {}
static inline void trace_usb_host_req_data(int bus, int addr, void *p, int in, int ep, int size) {}
static inline void trace_usb_host_req_complete(int bus, int addr, void *p, int status, int length) {}
static inline void trace_usb_host_req_emulated(int bus, int addr, void *p, int status) {}
static inline void trace_usb_host_req_canceled(int bus, int addr, void *p) {}
static inline void trace_usb_host_iso_start(int bus, int addr, int ep) {}
static inline void trace_usb_host_iso_stop(int bus, int addr, int ep) {}
static inline void trace_usb_host_iso_out_of_bufs(int bus, int addr, int ep) {}
static inline void trace_usb_host_reset(int bus, int addr) {}
static inline void trace_usb_host_parse_config(int bus, int addr, int value, int active) {}
static inline void trace_usb_host_parse_interface(int bus, int addr, int num, int alt, int active) {}
static inline void trace_usb_host_parse_endpoint(int bus, int addr, int ep, const char *dir, const char *type, int active) {}
static inline void trace_usb_host_parse_error(int bus, int addr, const char *errmsg) {}
static inline void trace_usb_host_remote_wakeup_removed(int bus, int addr) {}
static inline void trace_usb_serial_reset(int bus, int addr) {}
static inline void trace_usb_serial_handle_control(int bus, int addr, int request, int value) {}
static inline void trace_usb_serial_unsupported_parity(int bus, int addr, int value) {}
static inline void trace_usb_serial_unsupported_stopbits(int bus, int addr, int value) {}
static inline void trace_usb_serial_unsupported_control(int bus, int addr, int request, int value) {}
static inline void trace_usb_serial_unsupported_data_bits(int bus, int addr, int value) {}
static inline void trace_usb_serial_bad_token(int bus, int addr) {}
static inline void trace_usb_serial_set_baud(int bus, int addr, int baud) {}
static inline void trace_usb_serial_set_data(int bus, int addr, int parity, int data, int stop) {}
static inline void trace_usb_serial_set_flow_control(int bus, int addr, int index) {}
static inline void trace_usb_serial_set_xonxoff(int bus, int addr, uint8_t xon, uint8_t xoff) {}
static inline void trace_canokey_emu_stall_ep(uint8_t ep) {}
static inline void trace_canokey_emu_set_address(uint8_t addr) {}
static inline void trace_canokey_emu_prepare_receive(uint8_t ep, uint16_t size) {}
static inline void trace_canokey_emu_transmit(uint8_t ep, uint16_t size) {}
static inline void trace_canokey_handle_control_setup(int request, int value, int index, int length) {}
static inline void trace_canokey_handle_control_in(int actual_len) {}
static inline void trace_canokey_handle_data_out(uint8_t ep_out, uint32_t out_len) {}
static inline void trace_canokey_handle_data_in(uint8_t ep_in, uint32_t in_len) {}
