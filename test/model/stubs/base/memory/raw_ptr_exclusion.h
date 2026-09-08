// Host-test shim for base/memory/raw_ptr_exclusion.h. The real header
// reaches partition_alloc's GENERATED buildflags, which no host build has;
// the macro is the only thing the client's host-clean headers
// (phi_space_manager.h, phi_url_router.h) take from it. run.sh puts this
// directory ahead of the fork checkout on the include path.
#ifndef PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_MEMORY_RAW_PTR_EXCLUSION_H_
#define PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_MEMORY_RAW_PTR_EXCLUSION_H_

#define RAW_PTR_EXCLUSION

#endif  // PHIBROWSER_WIN_TEST_MODEL_STUBS_BASE_MEMORY_RAW_PTR_EXCLUSION_H_
