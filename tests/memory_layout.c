/* Validate the actual platform HOBs and MMU map; only trace/HOB sinks mocked. */
#include <PiPei.h>
/* Use ARM descriptor declarations with the host's already selected 64-bit ABI. */
#define MDE_CPU_AARCH64
#include <Library/ArmLib.h>
#undef MDE_CPU_AARCH64
#include <Library/ArmPlatformLib.h>
#include <Library/HobLib.h>
#define W202_LK_TRACE_H
static VOID LkTraceStage(UINT64 Stage) { (void)Stage; }
#include "../Ums9620Pkg/W202ds/Library/LkInheritedPlatformLib/ArmPlatformLibNull.c"
#include "../Ums9620Pkg/W202ds/Library/LkInheritedPlatformLib/ArmPlatformLibNullMem.c"

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static unsigned resources, allocations;
static EFI_RESOURCE_TYPE resource_type;
static EFI_RESOURCE_ATTRIBUTE_TYPE attributes;
static EFI_PHYSICAL_ADDRESS resource_base, reserved_base;
static UINT64 resource_size, reserved_size;
static EFI_MEMORY_TYPE reserved_type;

VOID EFIAPI BuildResourceDescriptorHob(EFI_RESOURCE_TYPE Type,
    EFI_RESOURCE_ATTRIBUTE_TYPE Attributes, EFI_PHYSICAL_ADDRESS Base, UINT64 Size) {
  ++resources; resource_type=Type; attributes=Attributes;
  resource_base=Base; resource_size=Size;
}

VOID EFIAPI BuildMemoryAllocationHob(EFI_PHYSICAL_ADDRESS Base, UINT64 Size,
    EFI_MEMORY_TYPE Type) {
  ++allocations; reserved_base=Base; reserved_size=Size; reserved_type=Type;
}

int main(void) {
  ARM_MEMORY_REGION_DESCRIPTOR *map;
  UINTN i;
  UINT64 bootstrap_size=0;
  unsigned matching_map=0, os_staging=0, lk_service=0, framebuffer=0;
  const UINT64 excluded[][2]={
    {0x82800000,0x86000000}, /* Existing bootstrap bank. */
    {0x86000000,0xae000000}, /* LK download buffer, SIPC and modem. */
    {0xae600000,0xae900000}, {0xaf700000,0xb09e4000}, /* Audio/display. */
    {0xb5000000,0xb5f00000}, {0xbbe80000,0xbbf00000}, /* LK/log. */
    {0xbd000000,0xbd040000}, {0xbd060000,0xbe900000}, /* SML/TOS. */
    {0xc3000000,0xc3100000}, /* Shared memory. */
    {0xd0000000,0xd8200000}  /* OS staging: never allocator RAM. */
  };
  CHECK(ArmPlatformGetBootMode()==BOOT_WITH_FULL_CONFIGURATION);
  CHECK(resources==1 && resource_type==EFI_RESOURCE_SYSTEM_MEMORY);
  CHECK((attributes & (EFI_RESOURCE_ATTRIBUTE_PRESENT |
      EFI_RESOURCE_ATTRIBUTE_INITIALIZED | EFI_RESOURCE_ATTRIBUTE_TESTED |
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE))==
      (EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
       EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE));
  CHECK((resource_base & 0xfff)==0 && (resource_size & 0xfff)==0);
  CHECK(resource_size!=0 && resource_base+resource_size>resource_base);
  CHECK(allocations==1 && reserved_base==0x82ffe000 && reserved_size==0x2000 &&
        reserved_type==EfiReservedMemoryType);
  for(i=0;i<sizeof(excluded)/sizeof(excluded[0]);++i)
    CHECK(resource_base>=excluded[i][1] || resource_base+resource_size<=excluded[i][0]);
  ArmPlatformGetVirtualMemoryMap(&map);
  for(i=0;map[i].Length;++i) {
    if(map[i].PhysicalBase==0xb0000000) {
      ++framebuffer;
      CHECK(map[i].VirtualBase==0xb0000000 && map[i].Length==0x9e4000);
      CHECK(map[i].Length>=1200*1920*4 &&
            map[i].Attributes==ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED);
    }
    if(map[i].PhysicalBase==0x82800000) bootstrap_size=map[i].Length;
    if(map[i].PhysicalBase==resource_base) {
      ++matching_map;
      CHECK(map[i].VirtualBase==resource_base && map[i].Length==resource_size &&
            map[i].Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK);
    }
    if(map[i].PhysicalBase==0xd0000000) {
      ++os_staging;
      CHECK(map[i].Length==0x08200000);
    }
    if(map[i].PhysicalBase==0x86000000 || map[i].PhysicalBase==0x86400000) {
      ++lk_service;
      CHECK(map[i].VirtualBase==map[i].PhysicalBase && map[i].Length==0x40000);
      CHECK(map[i].Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK);
    }
  }
  CHECK(matching_map==1 && os_staging==1 && framebuffer==1);
  CHECK(bootstrap_size==56ULL*1024*1024);
  CHECK(bootstrap_size+resource_size==128ULL*1024*1024);
#ifdef W202_LK_FASTBOOT
  CHECK(lk_service==2);
#else
  CHECK(lk_service==0);
#endif
  return 0;
}
