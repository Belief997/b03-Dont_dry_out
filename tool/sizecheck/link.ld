/* Flash budget measurement only -- NOT a build artifact of this project.
 *
 * The project is MDK-only and Keil is not installed on this machine, so the
 * authoritative armcc size is unavailable. This script reproduces the .uvprojx
 * memory layout so arm-none-eabi-gcc can produce a real *linked* size, which is
 * a far better estimate than summing .o files (that counts everything the
 * linker would have discarded).
 *
 * Memory layout copied verbatim from ble_app_beacon_pca10040e_s112.uvprojx:
 *     OCR_RVCT4 (IROM1): StartAddress 0x19000     Size 0x13000
 *     OCR_RVCT9 (IRAM1): StartAddress 0x20002500  Size 0x3b00
 *
 * 0x19000 is where the S112 SoftDevice ends; 0x19000 + 0x13000 = 0x2C000 is
 * exactly where app_storage's config page starts. So the region measured here is
 * bounded by the storage partition by design -- see app_storage.h.
 *
 * The SECTIONS blocks below are the SDK's "section variables" mechanism
 * (nrf_section_iter). Without them the link fails with undefined
 * __start_/__stop_ symbols. Only the 9 sections this firmware actually
 * references are listed; the shape is copied from
 * examples/ble_peripheral/ble_app_alert_notification/.../armgcc/*.ld.
 */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x19000, LENGTH = 0x13000
  RAM (rwx) :  ORIGIN = 0x20002500, LENGTH = 0x3b00
}

SECTIONS
{
}

/* RAM-resident section variables. */
SECTIONS
{
  . = ALIGN(4);
  .mem_section_dummy_ram :
  {
  }
  .log_dynamic_data :
  {
    PROVIDE(__start_log_dynamic_data = .);
    KEEP(*(SORT(.log_dynamic_data*)))
    PROVIDE(__stop_log_dynamic_data = .);
  } > RAM
  .fs_data :
  {
    PROVIDE(__start_fs_data = .);
    KEEP(*(.fs_data))
    PROVIDE(__stop_fs_data = .);
  } > RAM

} INSERT AFTER .data;

/* Flash-resident section variables. */
SECTIONS
{
  .mem_section_dummy_rom :
  {
  }
  .sdh_soc_observers :
  {
    PROVIDE(__start_sdh_soc_observers = .);
    KEEP(*(SORT(.sdh_soc_observers*)))
    PROVIDE(__stop_sdh_soc_observers = .);
  } > FLASH
  .sdh_ble_observers :
  {
    PROVIDE(__start_sdh_ble_observers = .);
    KEEP(*(SORT(.sdh_ble_observers*)))
    PROVIDE(__stop_sdh_ble_observers = .);
  } > FLASH
  .sdh_state_observers :
  {
    PROVIDE(__start_sdh_state_observers = .);
    KEEP(*(SORT(.sdh_state_observers*)))
    PROVIDE(__stop_sdh_state_observers = .);
  } > FLASH
  .sdh_stack_observers :
  {
    PROVIDE(__start_sdh_stack_observers = .);
    KEEP(*(SORT(.sdh_stack_observers*)))
    PROVIDE(__stop_sdh_stack_observers = .);
  } > FLASH
  .sdh_req_observers :
  {
    PROVIDE(__start_sdh_req_observers = .);
    KEEP(*(SORT(.sdh_req_observers*)))
    PROVIDE(__stop_sdh_req_observers = .);
  } > FLASH
  .pwr_mgmt_data :
  {
    PROVIDE(__start_pwr_mgmt_data = .);
    KEEP(*(SORT(.pwr_mgmt_data*)))
    PROVIDE(__stop_pwr_mgmt_data = .);
  } > FLASH
  .log_const_data :
  {
    PROVIDE(__start_log_const_data = .);
    KEEP(*(SORT(.log_const_data*)))
    PROVIDE(__stop_log_const_data = .);
  } > FLASH
  .nrf_balloc :
  {
    PROVIDE(__start_nrf_balloc = .);
    KEEP(*(.nrf_balloc))
    PROVIDE(__stop_nrf_balloc = .);
  } > FLASH

} INSERT AFTER .text

INCLUDE "nrf_common.ld"
