// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "lib/acpi_tables.h"

#include <assert.h>
#include <err.h>
#include <lib/acpi_lite.h>
#include <trace.h>

#include <fbl/span.h>
#include <lk/init.h>

#define LOCAL_TRACE 0

namespace {

// Read a POD struct of type "T" from memory at "data" with a given
// offset.
//
// Return the struct, and a span of the data which may be larger than
// sizeof(T).
//
// Return an error if the span is not large enough to contain the data.
template <typename T>
inline zx_status_t ReadStruct(fbl::Span<const uint8_t> data, T* out, size_t offset = 0) {
  // Ensure there is enough data for the header.
  if (data.size_bytes() < offset + sizeof(T)) {
    return ZX_ERR_INTERNAL;
  }

  // Copy the data to the out pointer.
  //
  // We copy the memory instead of just returning a raw pointer to the
  // input data to avoid unaligned memory accesses, which are undefined
  // behaviour in C (albeit, on x86 don't tend to matter in practice).
  memcpy(reinterpret_cast<uint8_t*>(out), data.data() + offset, sizeof(T));

  return ZX_OK;
}

// Read a POD struct of type "T" from memory at "data", which has a length field "F".
//
// Return the struct, and a span of the data which may be larger than
// sizeof(T).
//
// Return an error if the span is not large enough to contain the data.
template <typename T, typename F>
inline zx_status_t ReadVariableLengthStruct(fbl::Span<const uint8_t> data, F T::*length_field,
                                            T* out, fbl::Span<const uint8_t>* payload,
                                            size_t offset = 0) {
  // Read the struct data.
  zx_status_t status = ReadStruct(data, out, offset);
  if (status != ZX_OK) {
    return status;
  }

  // Ensure the input data is large enough to fit the data in
  // "length_field".
  auto length = static_cast<size_t>(out->*length_field);
  if (length + offset > data.size_bytes()) {
    return ZX_ERR_INTERNAL;
  }

  // Create a span of the data.
  *payload = fbl::Span<const uint8_t>(data.begin() + offset, out->*length_field);

  return ZX_OK;
}

// Read an ACPI table entry of type "T" from the memory starting a "header".
//
// On success, we return the struct and a span refering to the range of
// data, which may be larger than sizeof(T).
//
// We assume that the memory being pointed contains at least sizeof(T)
// bytes. Return an error if the header isn't valid.
template <typename T>
inline zx_status_t ReadAcpiEntry(const acpi_sdt_header* header, T* out,
                                 fbl::Span<const uint8_t>* payload) {
  // Read the length. Use "memcpy" to avoid unaligned reads.
  uint32_t length;
  memcpy(&length, &header->length, sizeof(uint32_t));
  if (length < sizeof(T)) {
    return ZX_ERR_INTERNAL;
  }

  // Ensure the table doesn't wrap the address space.
  auto start = reinterpret_cast<uintptr_t>(header);
  auto end = start + header->length;
  if (end < start) {
    return ZX_ERR_INTERNAL;
  }

  // Ensure that the header length looks reasonable.
  if (header->length > 16 * 1024) {
    TRACEF("Table entry suspiciously long: %u\n", header->length);
    return ZX_ERR_INTERNAL;
  }

  // Read the result.
  *payload = fbl::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(header), length);
  return ReadStruct(*payload, out);
}

}  // namespace

zx_status_t AcpiTables::cpu_count(uint32_t* cpu_count) const {
  uint32_t count = 0;
  auto visitor = [&count](acpi_sub_table_header* record) {
    acpi_madt_local_apic_entry* lapic = (acpi_madt_local_apic_entry*)record;
    if (!(lapic->flags & ACPI_MADT_FLAG_ENABLED)) {
      LTRACEF("Skipping disabled processor %02x\n", lapic->apic_id);
      return ZX_OK;
    }

    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_LOCAL_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *cpu_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::cpu_apic_ids(uint32_t* apic_ids, uint32_t array_size,
                                     uint32_t* apic_id_count) const {
  uint32_t count = 0;
  auto visitor = [apic_ids, array_size, &count](acpi_sub_table_header* record) {
    acpi_madt_local_apic_entry* lapic = (acpi_madt_local_apic_entry*)record;
    if (!(lapic->flags & ACPI_MADT_FLAG_ENABLED)) {
      LTRACEF("Skipping disabled processor %02x\n", lapic->apic_id);
      return ZX_OK;
    }

    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }
    apic_ids[count++] = lapic->apic_id;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_LOCAL_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *apic_id_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::io_apic_count(uint32_t* io_apics_count) const {
  return NumInMadt(ACPI_MADT_TYPE_IO_APIC, io_apics_count);
}

zx_status_t AcpiTables::io_apics(io_apic_descriptor* io_apics, uint32_t array_size,
                                 uint32_t* io_apics_count) const {
  uint32_t count = 0;
  auto visitor = [io_apics, array_size, &count](acpi_sub_table_header* record) {
    acpi_madt_io_apic_entry* io_apic = (acpi_madt_io_apic_entry*)record;
    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }
    io_apics[count].apic_id = io_apic->io_apic_id;
    io_apics[count].paddr = io_apic->io_apic_address;
    io_apics[count].global_irq_base = io_apic->global_system_interrupt_base;
    count++;
    return ZX_OK;
  };
  auto status = ForEachInMadt(ACPI_MADT_TYPE_IO_APIC, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *io_apics_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::interrupt_source_overrides_count(uint32_t* overrides_count) const {
  return NumInMadt(ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE, overrides_count);
}

zx_status_t AcpiTables::interrupt_source_overrides(io_apic_isa_override* overrides,
                                                   uint32_t array_size,
                                                   uint32_t* overrides_count) const {
  uint32_t count = 0;
  auto visitor = [overrides, array_size, &count](acpi_sub_table_header* record) {
    if (count >= array_size) {
      return ZX_ERR_INVALID_ARGS;
    }

    acpi_madt_int_source_override_entry* iso = (acpi_madt_int_source_override_entry*)record;

    ASSERT(iso->bus == 0);  // 0 means ISA, ISOs are only ever for ISA IRQs
    overrides[count].isa_irq = iso->source;
    overrides[count].remapped = true;
    overrides[count].global_irq = iso->global_sys_interrupt;

    uint32_t flags = iso->flags;
    uint32_t polarity = flags & ACPI_MADT_FLAG_POLARITY_MASK;
    uint32_t trigger = flags & ACPI_MADT_FLAG_TRIGGER_MASK;

    // Conforms below means conforms to the bus spec.  ISA is
    // edge triggered and active high.
    switch (polarity) {
      case ACPI_MADT_FLAG_POLARITY_CONFORMS:
      case ACPI_MADT_FLAG_POLARITY_HIGH:
        overrides[count].pol = IRQ_POLARITY_ACTIVE_HIGH;
        break;
      case ACPI_MADT_FLAG_POLARITY_LOW:
        overrides[count].pol = IRQ_POLARITY_ACTIVE_LOW;
        break;
      default:
        panic("Unknown IRQ polarity in override: %u\n", polarity);
    }

    switch (trigger) {
      case ACPI_MADT_FLAG_TRIGGER_CONFORMS:
      case ACPI_MADT_FLAG_TRIGGER_EDGE:
        overrides[count].tm = IRQ_TRIGGER_MODE_EDGE;
        break;
      case ACPI_MADT_FLAG_TRIGGER_LEVEL:
        overrides[count].tm = IRQ_TRIGGER_MODE_LEVEL;
        break;
      default:
        panic("Unknown IRQ trigger in override: %u\n", trigger);
    }

    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *overrides_count = count;
  return ZX_OK;
}

zx_status_t AcpiTables::NumInMadt(uint8_t type, uint32_t* element_count) const {
  uint32_t count = 0;
  auto visitor = [&count](acpi_sub_table_header* record) {
    count++;
    return ZX_OK;
  };

  auto status = ForEachInMadt(type, visitor);
  if (status != ZX_OK) {
    return status;
  }

  *element_count = count;
  return ZX_OK;
}

template <typename V>
zx_status_t AcpiTables::ForEachInMadt(uint8_t type, V visitor) const {
  uintptr_t records_start, records_end;
  zx_status_t status = GetMadtRecordLimits(&records_start, &records_end);
  if (status != ZX_OK) {
    return status;
  }

  uintptr_t addr;
  acpi_sub_table_header* record_hdr;
  for (addr = records_start; addr < records_end; addr += record_hdr->length) {
    record_hdr = (acpi_sub_table_header*)addr;
    if (record_hdr->type == type) {
      status = visitor(record_hdr);
      if (status != ZX_OK) {
        return status;
      }
    }
  }

  if (addr != records_end) {
    TRACEF("malformed MADT\n");
    return ZX_ERR_INTERNAL;
  }
  return ZX_OK;
}

zx_status_t AcpiTables::GetMadtRecordLimits(uintptr_t* start, uintptr_t* end) const {
  acpi_sdt_header* table = nullptr;
  zx_status_t status = tables_->GetTable((char*)ACPI_MADT_SIG, (char**)&table);
  if (status != ZX_OK) {
    TRACEF("could not find MADT\n");
    return ZX_ERR_NOT_FOUND;
  }
  acpi_madt_table* madt = (acpi_madt_table*)table;
  uintptr_t records_start = ((uintptr_t)madt) + sizeof(*madt);
  uintptr_t records_end = ((uintptr_t)madt) + madt->header.length;
  if (records_start >= records_end) {
    TRACEF("MADT wraps around address space\n");
    return ZX_ERR_INTERNAL;
  }
  // Shouldn't be too many records
  if (madt->header.length > 4096) {
    TRACEF("MADT suspiciously long: %u\n", madt->header.length);
    return ZX_ERR_INTERNAL;
  }
  *start = records_start;
  *end = records_end;
  return ZX_OK;
}

zx_status_t AcpiTables::hpet(acpi_hpet_descriptor* hpet) const {
  acpi_sdt_header* table = NULL;
  zx_status_t status = tables_->GetTable((char*)ACPI_HPET_SIG, (char**)&table);
  if (status != ZX_OK) {
    TRACEF("could not find HPET\n");
    return ZX_ERR_NOT_FOUND;
  }

  acpi_hpet_table* hpet_tbl = (acpi_hpet_table*)table;
  if (hpet_tbl->header.length != sizeof(acpi_hpet_table)) {
    TRACEF("Unexpected HPET table length\n");
    return ZX_ERR_NOT_FOUND;
  }

  hpet->minimum_tick = hpet_tbl->minimum_tick;
  hpet->sequence = hpet_tbl->sequence;
  hpet->address = hpet_tbl->address.address;
  switch (hpet_tbl->address.address_space_id) {
    case ACPI_ADDR_SPACE_IO:
      hpet->port_io = true;
      break;
    case ACPI_ADDR_SPACE_MEMORY:
      hpet->port_io = false;
      break;
    default:
      return ZX_ERR_NOT_SUPPORTED;
  }

  return ZX_OK;
}

zx_status_t AcpiTables::debug_port(AcpiDebugPortDescriptor* desc) const {
  // Find the DBG2 table entry.
  acpi_sdt_header* table;
  zx_status_t status = tables_->GetTable((char*)ACPI_DBG2_SIG, (char**)&table);
  if (status != ZX_OK) {
    TRACEF("acpi: could not find debug port (v2) ACPI entry\n");
    return ZX_ERR_NOT_FOUND;
  }

  // Read the DBG2 header.
  acpi_dbg2_table debug_table;
  fbl::Span<const uint8_t> payload;
  status = ReadAcpiEntry(table, &debug_table, &payload);
  if (status != ZX_OK) {
    TRACEF("acpi: Failed to read DBG2 ACPI header.\n");
    return status;
  }

  // Ensure at least one debug port.
  if (debug_table.num_entries < 1) {
    TRACEF("acpi: DBG2 table contains no debug ports.\n");
    return ZX_ERR_NOT_FOUND;
  }

  // Read the first device payload.
  acpi_dbg2_device device;
  fbl::Span<const uint8_t> device_payload;
  status = ReadVariableLengthStruct<acpi_dbg2_device>(payload,
                                                      /*length_field=*/&acpi_dbg2_device::length,
                                                      /*out=*/&device,
                                                      /*payload=*/&device_payload,
                                                      /*offset=*/debug_table.offset);
  if (status != ZX_OK) {
    TRACEF("acpi: Could not parse DBG2 device.\n");
    return status;
  }

  // Ensure we are a supported type.
  if (device.port_type != ACPI_DBG2_TYPE_SERIAL_PORT ||
      device.port_subtype != ACPI_DBG2_SUBTYPE_16550_COMPATIBLE) {
    TRACEF("acpi: DBG2 debug port unsuported. (type=%x, subtype=%x)\n", device.port_type,
           device.port_subtype);
    return ZX_ERR_NOT_SUPPORTED;
  }

  // We need at least one register.
  if (device.register_count < 1) {
    TRACEF("acpi: DBG2 debug port doesn't have any registers defined.\n");
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Get base address and length.
  acpi_generic_address_ address;
  status = ReadStruct(device_payload, &address, /*offset=*/device.base_address_offset);
  if (status != ZX_OK) {
    TRACEF("acpi: Failed to read DBG2 address registers.\n");
    return status;
  }
  uint32_t address_length;
  status = ReadStruct(device_payload, &address_length, /*offset=*/device.address_size_offset);
  if (status != ZX_OK) {
    TRACEF("acpi: Failed to read DBG2 address length.\n");
    return status;
  }

  // Ensure we are a MMIO address.
  if (address.address_space_id != ACPI_ADDR_SPACE_MEMORY) {
    TRACEF("acpi: Address space unsupported (space_id=%x)\n", address.address_space_id);
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Return information.
  desc->address = static_cast<paddr_t>(address.address);

  return ZX_OK;
}

zx_status_t AcpiTables::VisitCpuNumaPairs(
    fbl::Function<void(const AcpiNumaDomain&, uint32_t)> visitor) const {
  acpi_sdt_header* table = NULL;
  zx_status_t status = tables_->GetTable((char*)ACPI_SRAT_SIG, (char**)&table);
  if (status != ZX_OK) {
    printf("Could not find SRAT table. Get table returned: %d\n", status);
    return ZX_ERR_NOT_FOUND;
  }

  acpi_srat_table* srat = (acpi_srat_table*)table;

  static constexpr size_t kSratHeaderSize = 48;
  static constexpr size_t kMaxNumaDomains = 10;
  AcpiNumaDomain domains[kMaxNumaDomains];

  // First find all numa domains.
  size_t offset = kSratHeaderSize;
  while (offset < srat->header.length) {
    acpi_sub_table_header* sub_header = (acpi_sub_table_header*)((uint64_t)table + offset);
    DEBUG_ASSERT(sub_header->length > 0);
    offset += sub_header->length;
    if (sub_header->type == ACPI_SRAT_TYPE_MEMORY_AFFINITY) {
      const acpi_srat_memory_affinity_entry* mem = (acpi_srat_memory_affinity_entry*)sub_header;
      if (!(mem->flags & ACPI_SRAT_FLAG_ENABLED)) {
        // Ignore disabled entries.
        continue;
      }

      DEBUG_ASSERT(mem->proximity_domain < kMaxNumaDomains);

      auto& domain = domains[mem->proximity_domain];
      uint64_t base = ((uint64_t)mem->base_address_high << 32) | mem->base_address_low;
      uint64_t length = ((uint64_t)mem->length_high << 32) | mem->length_low;
      domain.memory[domain.memory_count++] = {.base_address = base, .length = length};

      printf("ACPI SRAT: numa Region:{ domain: %u base: %#lx length: %#lx (%lu) }\n",
             mem->proximity_domain, base, length, length);
    }
  }

  // Then visit all cpu apic ids and provide the accompanying numa region.
  offset = kSratHeaderSize;
  while (offset < srat->header.length) {
    acpi_sub_table_header* sub_header = (acpi_sub_table_header*)((uint64_t)table + offset);
    offset += sub_header->length;
    const auto type = sub_header->type;
    if (type == ACPI_SRAT_TYPE_PROCESSOR_AFFINITY) {
      const auto* cpu = (acpi_srat_processor_affinity_entry*)sub_header;
      if (!(cpu->flags & ACPI_SRAT_FLAG_ENABLED)) {
        // Ignore disabled entries.
        continue;
      }
      uint32_t domain = cpu->proximity_domain_low;
      domain |= cpu->proximity_domain_high[0] << 8;
      domain |= cpu->proximity_domain_high[1] << 16;
      domain |= cpu->proximity_domain_high[2] << 24;

      DEBUG_ASSERT_MSG(domain < kMaxNumaDomains, "%u < %lu", domain, kMaxNumaDomains);
      domains[domain].domain = domain;
      visitor(domains[domain], cpu->apic_id);

    } else if (type == ACPI_SRAT_TYPE_PROCESSOR_X2APIC_AFFINITY) {
      const auto* cpu = (acpi_srat_processor_x2apic_affinity_entry*)sub_header;
      if (!(cpu->flags & ACPI_SRAT_FLAG_ENABLED)) {
        // Ignore disabled entries.
        continue;
      }

      DEBUG_ASSERT(cpu->proximity_domain < kMaxNumaDomains);
      visitor(domains[cpu->proximity_domain], cpu->x2apic_id);
    }
  }

  return ZX_OK;
}

// Wraps ACPICA functions (except init) to allow testing.
// Looks up table, on success sets header to point to table. Maintains
// ownership of the table's memory.
zx_status_t AcpiTableProvider::GetTable(char* signature, char** header_out) const {
  auto header = acpi_get_table_by_sig(signature);
  if (header) {
    *header_out = (char*)header;
    return ZX_OK;
  }
  return ZX_ERR_NOT_FOUND;
}
