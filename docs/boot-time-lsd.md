# Boot time: why lsd seeds its database at every boot, and how to stop paying for it

Status: analysis and specification only. Nothing was built, run, committed or sent to the device. Written 2026-11-18.
Follows `docs/app-launch-latency.md` (F3) and HANDOFF-2026-11-17-night.md ("Boot time and launch", runs lsa1-3).

Labels: **[V]** verified (address and instruction, file:line, or a log line with its time). **[I]** inference, with
its basis and how sure. **[G]** guess. Times in brackets are kernel seconds from the console part of a log. All
addresses are unslid iOS 16.5 (20F66, iPhone10,3) dyld-cache addresses. The cache's local symbols are redacted, so
C functions are named here by what they do (in `backticks`, my names), Objective-C methods by their real names.

How the binary facts were found: `tools/dsc/dsc.py` (cache reader) and `tools/dsc/objcimp.py`'s `scan()` (method
names), plus a scratch script that (1) finds a string in an image's `__cstring`/`__oslogstring` and the `__cfstring`
records pointing at it, (2) scans the image's `__text` for `adrp`+`add`/`ldr` pairs reaching either address, (3) takes the
containing function from `LC_FUNCTION_STARTS`, (4) disassembles with capstone, resolving `bl` through `__objc_stubs`
(selector) and branch islands (target symbol). Every address in §1 was found this way, from a log string or a
selector name. `lsd` itself is a 4-byte `__text` that calls `_LSServerMain` (CoreServices): all the logic is in
CoreServices (`/usr/libexec/lsd` imports only `__LSServerMain`) **[V]**.

## 0. Answers in short

1. **On real iOS, lsd skips the full scan when its saved store loads, is "current" and is "seeded complete".**
   - The store is `<lsd's data container>/Library/Caches/com.apple.LaunchServices-4035-v2.csstore`. The container is
     the internal-daemon container of user mobile: `/private/var/mobile/Containers/Data/InternalDaemon/<UUID>/` **[V]**
     for the name and query; **[I, high]** for the class-10 = InternalDaemon mapping.
   - At startup `-[LSDatabaseBuilder createAndSeedLocalDatabase:]` (0x181c8af3c) loads it. It skips the seed only if
     two things hold. The DB header's build and version must equal the OS's (`_LSDatabaseIsCurrent`, 0x181bfe2f4).
     The header's "seeding incomplete" bit must be clear (`_LSDatabaseSeedingComplete`, 0x181c12cb4) **[V]**.
   - There is no boot UUID, session UUID or LastBuildInfo in that decision. MobileInstallation's LastBuildInfo.plist
     belongs to installd alone **[V]**.
2. **On our port the seed runs at every boot for two independent reasons. Either one alone is enough.**
   - **C1, RAM /var (every test run today):** the container is on the memory filesystem. So there is never a store to
     load **[V]**. The earlier search looked in `/private/var/db` and `/private/var/mobile/Library/Caches`, which is
     the wrong place (psgrep.c:107) **[V]**.
   - **C2, every boot, including a saved /var:** the seed ends with an XPC "upcall" to installd to register
     containerized apps: `-[ICLWorkspace triggerRegistrationForContainerizedContentWithOptions:completion:]`.
     - installd is not one of our launchd jobs (build.sh:107-114) **[V]**. So the call fails, and lsd logs
       "PartialResultsReturned = YES, setting incomplete seed flag".
     - It then calls `setSeedingComplete:NO`, which also blanks the stored build and version. So the next boot
       re-seeds even when the store file is there. The code path is **[V]**; that this error happens is **[I, high]**.
       No log captures lsd's os_log.
     - This matches persist2/3, which seeded with a saved /var.
   - **The persist logs cannot say whether the csstore existed at the second boot.** No run ever listed the container
     **[V]**.
   - **A third possible cause, C3 [G, low-medium]:** the store write may fail on ramfs. The atomic replace may use
     `renamex_np(RENAME_SWAP)`, and ramfs answers that with ENOTSUP. D5 checks it.
3. **The ~0.23 s "per bundle" is per app. Per scanned bundle (apps plus their extensions plus framework plug-ins,
   about 690) it is about 0.09 s wall and 0.06 s CPU [I].** It is not our AMFI: code signatures are checked in lsd's
   own process by libmis/Security **[V]**.
   - 39 busy-thread samples (lsa2 + lsa3) split three ways **[V]**:
     - 41% blocked in synchronous XPC: containermanagerd's `container_set_info_value`,
       `container_set_code_signing_info_for_identifier` and data-container calls, plus usermanagerd's persona list;
     - 46% in filesystem syscalls (`getattrlist`/`realpath`, `getdirentries`, `open`/`close`, `pread`);
     - 10% in CPU work, mostly CFBundle and Info.plist parsing.
   - The per-bundle cost can be cut a little without caching: memoize the persona list, measure the APFS driver.
     The big wins come from not seeding at all, or seeding fewer bundles.
4. **Options, ranked by gain per unit of effort (§4):**
   - **R1:** answer the installd upcall in lsd. This is required by every caching fix.
   - **R2:** start lsd at load. About 20-25 s off every boot.
   - **R3:** hide 82 unused /Applications bundles from the scanner (33% of bundles), about 20 s.
   - **R4:** a read-only "golden" /var with a complete store, about 55-60 s. It needs one store write, which is
     forbidden today.
   - A prebuilt csstore in the boot ramdisk is not recommended (size, stale container paths). Lighter signature checks
     are not recommended. SpringBoard cannot start before the seed ends: lsd's own XPC handlers wait on the seeding
     group (0x181c00808) **[V]**.
   - **The top option, the full design in §4.2, is R1+R2+R3: lsd injected with libredwood, started at load,
     scanning a trimmed /Applications.** None of it is forbidden. Expected SpringBoard first frame: ~80-85 s against
     ~130 s today **[I, low-medium]**. R4 would bring it to ~75 s once the owner allows one store write.
5. **One silent diagnostic run, D5 (§5).**
   - It logs the store's state at lsd's start, the seed's outcome, the upcall's error, and per-category bundle costs.
   - It answers the upcall as success, so the seed completes.
   - Then, in the same boot, it restarts lsd. The restarted lsd must load the store it just saved without seeding.
     This proves C1/C2/C3 with no persistent write.
   - No camera, no shutter, no lock button, no sound_test.

## 1. Question 1: what lets lsd skip the scan on iOS 16.5

### 1.1 Where the store is **[V]**

| What | Address | What it does | Found from |
|---|---|---|---|
| `-[_LSDefaults databaseStoreFileURLWithUID:]` | 0x181c924b0 | `[[self databaseContainerURLWithUID:] URLByAppendingPathComponent: [NSString stringWithFormat:@"%s-%03llu-v2.csstore", getenv("LS_NAME_PREFIX") ?: "com.apple.LaunchServices", currentSchemaVersion]]`, with `System-` in front when the uid is 0 | the format string |
| `-[_LSDefaults currentSchemaVersion]` | 0x181be0bcc | `mov w0, #0xfc3`: **4035** | |
| `-[_LSDefaults databaseContainerURL]` | 0x181c92460 | `systemContainerURL` in education mode or a lightweight system server, otherwise `userContainerURL` | |
| `userContainerURL` block | 0x181c91ff0 | Only when `isServer` (byte +0x4b, `-[_LSDefaults isServer]` 0x181be07b8): `container_query_create`, `set_class(10)`, `set_persona_unique_string`, `operation_set_flags(0x900000001)`, `get_single_result`, `container_get_path` + `Library/Caches`, consumes the sandbox token, `createDirectoryAtURL` | |
| `systemContainerURL` block | 0x181c91f08 → 0x181c91cd8 | `getenv("LS_DATABASE_DIR")`, else `container_system_path_for_identifier("com.apple.lsd")`. Not used by our lsd: it is neither in education mode nor the lightweight system server | |
| Sibling files in the same directory | `dbSentinelFileURL` 0x181c92760, `dbRecoveryFileURL` 0x181c927e8, `dbSyncInterruptedFileURL` 0x181c9282c | `com.apple.LaunchServices.dirty`, `.error`, `.syncInterrupted` | |
| System-content snapshot | `systemContentDatabaseStoreFileURLWithUID:` 0x181c925a4 | `SystemDataOnly-<store name>`, saved after the built-in phase ("Saved system content database to %@", 0x181c53e24) | |

- Our lsd job runs as `UserName mobile`, with an empty `EnvironmentVariables` (userland/root/LaunchDaemons/com.apple.lsd.plist) **[V]**.
- Class 10 with no identifier is the caller's own data container. libsystem_containermanager has the strings
  `Containers/Data/InternalDaemon` and "CONTAINER_CLASS_INTERNAL_DAEMON_DATA containers cannot be used ..."
  **[V strings, I mapping, high]**. So the path is `/private/var/mobile/Containers/Data/InternalDaemon/<UUID>/Library/Caches/com.apple.LaunchServices-4035-v2.csstore`.
  The UUID comes from containermanagerd.

### 1.2 The startup decision **[V]**

`__LSServerMain` (0x181ce936c), in order:
1. "%{public}s starting with schema version %llu for effective user %llu" (0x181ce94b8).
2. `sem_open("com.apple.lsd.firstboot")` (0x181ce9ecc). If it is missing ("first start after reboot"), old preference
   files are migrated. Later it logs "Reboot detected, performing first boot work" and does that work (0x181c3ea18):
   clear `LSLastSeenImportantAppRestrictionStateKey`, a few ms. Then it creates the semaphore. The semaphore is per
   boot, so this runs at every boot, and it is cheap.
3. `syncInterrupted` file? (0x181c8790c) → if it exists, log "Previous sync was interrupted, presumably by a
   reboot..." and remove the recovery file (0x181ce9958).
4. In a `dispatch_barrier_sync` (block 0x181cea2cc) it calls `-[LSDatabaseBuilder createAndSeedLocalDatabase:&didSeed]`.
   If nothing was seeded, it calls `seedCryptexContentIfNeeded:`.
5. If no seed was done, and either step 3 found the file or `_LSDatabaseIsDirty` (0x181c876f8: a global, or
   `com.apple.LaunchServices.dirty` exists), it queues an asynchronous rebuild (block 0x181cea358 → 0x181c55318).

`-[LSDatabaseBuilder createAndSeedLocalDatabase:]` (0x181c8af3c):
- `_LSDatabaseLoadOrCreate(7, &flags, &err)` (0x181c83f64):
  - If `com.apple.LaunchServices.error` exists, it logs "_LSCreateStoreWithFileContents found a recovery file, so
    creating a new empty store".
  - Otherwise it creates that recovery file, then `_LSDatabaseCreateFromPersistentStore` (0x181c84458) →
    `_LSCreateStoreWithFileContents` (0x181c84b0c) → `_CSStoreCreateWithURL` (0x1a3a80918), then removes the recovery
    file (0x181c840f0).
- Then **`if (db && _LSDatabaseIsCurrent(db) && _LSDatabaseSeedingComplete(db)) { *didSeed = 0; done }`**
  (0x181c8afb4, 0x181c8afc0).
- Otherwise it logs "LSDatabaseBuilder: Seeding database with UID: %d, EUID %d" and calls
  `_LSDatabaseSetSeedingComplete(db, NO)` (0x181c8b098). It sets `*didSeed = 1`, enters the seeding group
  (0x181c8b0b4) and starts the seed block (0x181c8b1e0) asynchronously.

`_LSDatabaseIsCurrent` (0x181bfe2f4):
- It reads the 0xd0-byte "DB Header" (`_LSDatabaseGetHeader` 0x181bf632c, `_CSStoreGetHeader`; "Failed to read DB
  header, generating new one").
- It compares the header's build string (+0x60, 16 bytes) with the OS build, and the header's `LSVersionNumber` (+8)
  with the OS version, using `_LSVersionNumberCompare`.
- If they differ, it logs "Local database thinks system build version is %s, but the OS really is %s." or "...system
  version is %@...". An empty OS build gives "Could not get OS build version." and returns NO.
- The OS values come from a dispatch_once (0x181cf82a4): `_CFCopySystemVersionPlatformDictionary`, which is the root's
  SystemVersion.plist, `ProductVersion` and `ProductBuildVersion`. That function also reads
  `/private/preboot/Cryptexes/OS/SystemVersion.plist` for the "cryptex version". The model code and cryptex version are
  written into the header but **not compared** here.

`_LSDatabaseSeedingComplete` (0x181c12cb4) returns NO, logging "Database seeding is incomplete, need to rebuild", when
header +0x58 bit 1 is set.

`_LSDatabaseSetSeedingComplete(db, ?, complete)` (0x181c834d0):
- With `complete`, it writes the current version, build, model code (0x181c837b0), cryptex version (0x181cf8004) and a
  second 16-byte string (0x181c838d8) into the header.
- Without, it writes `kLSVersionNumberNull` and empty strings. Either way it then calls `_LSDatabaseWriteHeader`
  (0x181c833dc → `_CSStoreWriteToHeader`).
- Called from `-[LSDatabaseBuilder setSeedingComplete:]` (0x181c8b4d8), which also posts
  `LSNotificationDatabaseSeedingComplete`.

The seed (block 0x181c8b1e0 → 0x181c534fc → 0x181c53710):
- "calling into InstalledContentLibrary to register all content" → `-[ICLWorkspace
  enumerateBuiltInSystemContentWithBlock:error:]` (InstalledContentLibrary 0x1b117c444, in process). It first calls
  `container_invalidate_code_signing_cache`, then `MILaunchServicesDatabaseGatherer`.
- "Performing upcall to register containerized content." → block 0x181c53fb8 → `-[ICLWorkspace
  triggerRegistrationForContainerizedContentWithOptions:completion:]` (0x1b117c958). That is
  `_connectionToInstallationDaemonWithError:` + `remoteObjectProxyWithErrorHandler:`, i.e. XPC to installd. lsd waits
  on a semaphore with `DISPATCH_TIME_FOREVER` (0x181c54074).
- The completion (0x181c540a4) clears the success byte when given an error.
- If the result is 0, the seed block logs "LSDatabaseBuilder: MobileInstallation reported PartialResultsReturned = YES,
  setting incomplete seed flag" (0x181c8b270). Then it sends `[builder setSeedingComplete:result]` (0x181c8b360).
- The seed's end leaves the seeding group, and clients wait for it:
  `-[_LSDOpenClient willHandleInvocation:isReply:]` (0x181c00808) calls `dispatch_group_wait` on it. That is why
  SpringBoard waits.

Saving: `_LSDatabaseSave` (0x181c89de4) → `_CSStoreWriteToURL` (CoreServicesStore 0x1a3a8099c):
- `open_dprotected_np` of a temporary `name(%llx~).csstore` in the same directory, then `setResourceValues:` (a failure
  there is "non-fatal").
- Then `-[NSFileManager replaceItemAtURL:withItemAtURL:backupItemName:options:resultingItemURL:error:]`, logging
  "Moved temporary CSStore file..." or "Failed atomic move of temporary CSStore file...". `_LSSessionSave` logs "saved
  database for session" or "error saving database for session %@: %@".

MobileInstallation's own state:
- `_MIRecordCurrentBuildVersion` (InstalledContentLibrary 0x1b1155c04, exported) writes the build info with
  `MI_writeToURL:`. It has no caller inside InstalledContentLibrary. Its user is installd (`lastBuildInfoFileURL`,
  "Reboot detected", SystemAppMigration in installd's strings) **[V]**.
- lsd's decision never reads it **[V]**. iOS 16.5 has no "install map" file on this path: lsd enumerates in process
  **[I]**.

## 2. Question 2: what fails on our port at every boot

| Condition | RAM /var (all current runs) | Saved /var (var_store=1, persist2/3) | Label |
|---|---|---|---|
| Store file at lsd start | **Never**: /private/var is a fresh ramfs (init.c:794), and the container is created during this boot | Kept if it was saved: var_store's SKIP list does not include `mobile/Containers` (var_store.c:76-84) | V / V |
| Container UUID stable | New each boot (backboardd's system container: 4B9E32A6... on 11-12, B81A3445... and 3C1F72D7... on 11-14, camgl/combo logs) | Expected stable: containermanagerd's cache under `/private/var/installd/Library/Caches/com.apple.containermanagerd` and the per-container metadata plists are restored together | V / I, medium |
| Build/version comparison | Cannot fail between two boots of one system image: both sides come from the read-only root's SystemVersion.plist | same | V |
| Boot or session UUID | Not part of the decision | same | V |
| Seeding-complete bit | Irrelevant: no file | **Fails at every seed**: the installd upcall errors (installd is not in IOS_JOBS, build.sh:107-114; its binary is in the image, system.manifest:58783) → `setSeedingComplete:NO` → header build/version blanked → next boot re-seeds | V (code), I high (runtime) |
| Data-protection class | `open_dprotected_np` on ramfs: `vn_create` → `vnode_setattr_fallback` ignores the unsupported `va_dataprotect_class` (xnu kpi_vfs.c:3089) | same | V |
| Atomic replace | ramfs has no `vnop_renamex` (ramfs.c:1696-1728 table; `VNOP_RENAMEX` returns the default's ENOTSUP). Whether Foundation's `replaceItemAtURL` uses `RENAME_SWAP` here and falls back is not known | same | V / G |
| Read-only root | Not involved: the store is under /private/var | same | V |
| Recovery, dirty or syncInterrupted files | Gone with the ramfs | Would force a rebuild only if the save caught lsd in the middle of a load or modification. The save is at run end, and lsd is idle then | V / I |
| Store saved by the end of the run | Seed ends ~113 s; var_store saves at ~190 s ("var_store: the run is ending", persist2 [192.35]) | — | V |

- **Did the csstore exist at the second boot?** The persist runs cannot say:
  - persist1/5 list `mobile/Library/SpringBoard`, `Containers/Data/Application` and `Containers/Shared/AppGroup`
    only;
  - persist2/3 kept logs give only "generation 68 ... 11772 entries, 64682952 bytes" and "generation 70 ... 11828
    entries, 69167120 bytes";
  - nothing lists `Containers/Data/InternalDaemon` **[V]**.
- C2 explains the seed with a saved /var whether the file was there or not **[I, high]**.
- lsd's timing (lsa2) **[V]**:
  - launched on demand at [47.92] (`memorystatus_update ... pid 36`); before that the sampler finds no lsd and reports
    pid 0;
  - CPU 0.62 s at [49.70] and 40.27 s at [112.7], then flat;
  - SpringBoard's `swap #2` at [129.73] (lsa3).
- While lsd works (57-114 s) the rest of the machine is idle: `!sample top` shows only lsd 47-71%, containermanagerd
  26-50% and usermanagerd 8-12% (lsa3) **[V]**. The seed is one serial chain of lsd → containermanagerd → usermanagerd,
  on a 4-core machine.
- Side observation, outside lsd: logd_helper at 71-87% of a core from [36.5] to [45.5] (lsa3) **[V]**.

## 3. Question 3: where the per-bundle time goes

How many bundles **[V]** (ios165/system.manifest):
- 173 `/Applications/*.app`;
- 230 `.appex` under them (DiagnosticsService.app alone has 64, News 12, MobileSlideShow 10);
- 285 `.appex` directly in framework `PlugIns`.
- lsd passes `OnlyScanPluginsUnderFrameworks` (0x181c535cc). The value was not decoded, so whether all 285 are
  scanned is **[I]**. That makes ~690 bundles in 63 s wall and 36.7 s thread CPU (25.3 user + 11.5 system, lsa2 t2):
  **~90 ms wall, ~55 ms CPU, ~17 ms system time each [I]**.

What each bundle costs (InstalledContentLibrary) **[V]**:
- `+[MILaunchServicesDatabaseGatherer entryForBundle:inContainer:withError:]` (0x1b1198170) calls:
  - `dataContainerCreatingIfNeeded:makeLive:created:error:` (0x1b1184b08): containermanagerd XPC; on a fresh /var it
    **creates** a data container for each app;
  - `-[MIContainerProtectionManager setDataProtectionOnDataContainer:forNewBundle:retryIfLocked:]` (0x1b115e1f0);
  - `bundleRecordWithError:`.
- `-[MIExecutableBundle codeSigningInfoByValidatingResources:performingOnlineAuthorization:ignoringCachedSigningInfo:checkingTrustCacheIfApplicable:skippingProfileIDValidation:error:]`
  (0x1b1183314):
  - first `_codeSigningInfoFromMCM` (0x1b1183174: `container_copy_code_signing_info_for_identifier`). It always misses
    at boot: the enumeration starts with `container_invalidate_code_signing_cache`;
  - then `MICodeSigningVerifier` → `+_validateSignatureAndCopyInfoForURL:withOptions:error:` (0x1b119599c) →
    **`libmis MISValidateSignatureAndCopyInfo`**, in lsd's process (Security `pread` frames in lsa3 +39);
  - resources are validated for removable system apps ("Validating %@ as removable system app");
  - then the result is written back with `container_set_code_signing_info_for_identifier` (lsa3 +60).
- `-[MIBundle _validateWithError:]` (0x1b1165d54) and `-[MIExecutableBundle _validateWithError:]` (0x1b1181854) are
  small: `infoPlistSubset` → `_MILoadInfoPlist` (0x1b115583c: `_CFBundleCreateUnique` +
  `_CFBundleCreateFilteredInfoPlist`), and `realPathForURL:ifChildOfURL:`.
  - The handoff's "`_validateWithError:`" comes from objcimp, which names the nearest method at or before an address,
    and from 12-frame stacks. It names where the stack was cut, not where the time went **[V]**.
- Personas: `-[UMXPCServer fetchListOfPersonasSynchronousWithError:]` → usermanagerd (lsa2 +85).

The 39 busy samples (lsa2 +34..+88, lsa3 +33..+90; slide per file) **[V]**:

| Where the thread was | Samples | Examples |
|---|---|---|
| Blocked in `mach_msg` (sync XPC) | 16 (41%) | `container_set_info_value`, `container_set_code_signing_info_for_identifier`, NSXPC `_sendInvocation` (×5), `fetchListOfPersonasSynchronous` |
| Filesystem syscalls | 18 (46%) | `getattrlist` (×5, `realpath`, CoreServicesInternal), `close` ×4, `getdirentries` ×2, `open` ×2, `pread` ×2 (Security, appex Mach-O versions), `read`, `readlink`, `posix_madvise` |
| User CPU | 4 (10%) | `_CFBundleCreateFilteredInfoPlist`/`CFPropertyListCreateFiltered`, NSArray, CF TSD |
| kevent | 1 | |

So:
- **Our AMFI stand-in is not the cost [I, high]:** signatures are checked in user space; the kernel is involved at most
  for a trust-cache query.
- **Info.plist parsing is a minor CPU share [V].**
- **The cost is the per-bundle XPC chain, ~4-6 synchronous calls, and filesystem metadata on the system image.**
  - The image is read by our own APFS driver, which decompresses zlib and LZBITMAP and takes a lock
    (xnu/files/bsd/miscfs/apfs).
  - It counts calls, decompression and lock waits in `kern.apfs_stats` (apfs_vfsops.c:270-330). No run has read it
    around the seed **[V]**.

Cuts that need no caching:

| Cut | Expected | Risk | Basis |
|---|---|---|---|
| Memoize usermanagerd's persona list in lsd for the seed | −5-10% (usermanagerd 8-12% of a core) | low | I |
| Speed the APFS driver's metadata path (name cache, getattrlist), after D5's `kern.apfs_stats` delta | up to part of the 31% system time | medium kernel work | G until measured |
| Skip data-container creation or code-signing during the seed | maybe −30% | **high**: LS records would lack entitlements and containers | not proposed |
| Skip `container_invalidate_code_signing_cache` | none on RAM /var (MCM's cache is empty anyway) | — | V |

## 4. Question 4: options

### 4.1 Ranking

| # | Option | Gain (boot → SpringBoard first frame) | Effort / risk | Needs forbidden? |
|---|---|---|---|---|
| R1 | **Answer the containerized upcall in lsd** (hook, §4.2), or load `com.apple.mobile.installd` | 0 s on RAM-/var boots on its own. Makes a saved store skip the seed (−55-60 s). Required by R4/R5 | XS / low. The hook only claims success when `/private/var/containers/Bundle/Application` is empty, which it is on every boot today. Loading installd instead is unknown: it does "Reboot detected" and SystemAppMigration work on a fresh /var | no |
| R2 | **Start lsd at load** (`RunAtLoad` in our job) | −20-25 s at every boot: the seed starts at ~21 s, not ~48 s, and the machine has idle cores [I, medium] | XS (mkjobs) / low: some CPU contention with SpringBoard's start and logd_helper | no |
| R3 | **Hide 82 unused /Applications bundles from the scanner** (hook, list file) | −33% of bundles ≈ −20 s of seed at every boot [I, medium] | S / medium: those apps vanish from LS. The list must keep the view services that core flows use | no |
| R2+R3 | together | seed ~21 → ~65-70 s; SpringBoard asks at ~57 s and waits ~10 s; first frame ~80-85 s [I, low-medium] | — | no |
| R8 | Persona memo; APFS metadata speed-up | −5-15% of the seed | S / low; M / medium | no |
| R4 | **Read-only golden /var**: one capture after a complete (R1) seed, then `var_store=ro` boots that load and never save | no seed: SpringBoard released at ~57 s, first frame ~75 s [I, medium]. The archive load time (65 MB) is not measured and is paid in init | S (var_store `ro` mode) / low | **one store write (the capture)** |
| R4' | var_store=1 as designed + R1 | as R4, plus everything else persists | none beyond R1 | **persistent writes** |
| R5 | Prebuilt csstore in the boot ramdisk, copied into lsd's container at boot | as R4 | L / high: size likely ≫ 1.4 MB free [G: iOS stores are typically 10+ MB; D5 measures ours]. It needs a fixed container UUID that containermanagerd adopts [G]. Its records hold data-container paths that a RAM boot does not recreate [I, medium]. It must be regenerated per build and pulled off the device | no, but impractical |
| R6 | Lighter per-bundle validation | ≤ −30% | high (entitlements and sandbox) | no; not proposed |
| R7 | SpringBoard before the seed ends | none possible: LS's handlers wait on the seeding group (0x181c00808) | — | — |

### 4.2 Top option, full design: `lsd` under libredwood, started at load, trimmed scan, complete seed

**Where.**
- `userland/src/inject.c` gets a new section "lsd", next to `container_manager_fixes` (inject.c:1990-2012). The
  constructor's dispatch on the program name (inject.c:3847) gains `else if (strcmp(name, "lsd") == 0) lsd_hooks();`.
- `userland/build.sh:155`: the `libredwood.dylib` INJECT line gains `lsd`. No new dylib, so nothing is added to the
  boot image beyond the code (~10 KB) and `etc/lsd-hide.txt` (~2 KB).
- Boot-args, read once: `lsdiag=1` (logs), `lsd_upcall=1` (R1), `lsd_hide=1` (R3). All default off until D5 and FT1
  pass; then R1 becomes default on (T6).
- Every hook uses the existing pattern: `class_getInstanceMethod`/`class_getClassMethod` +
  `method_setImplementation`, original IMP kept, `keep_line()` for kept lines.
- The classes live in CoreServices and InstalledContentLibrary, which lsd has mapped before our constructor runs:
  CoreServices references `ICLWorkspace` through a class ref (0x1dcf11010) **[I, high]**. If `objc_getClass`
  returns nil, register with `_dyld_register_func_for_add_image` and hook there, as inject_media.c does.

**R1, the upcall.** Replace `-[ICLWorkspace triggerRegistrationForContainerizedContentWithOptions:completion:]`
(InstalledContentLibrary 0x1b117c958; completion type `v16@?0@"NSError"8`):
```
orig(self, _cmd, options, ^(NSError *e) {
    if (lsdiag) keep_line("lsd: containerized upcall: error %.140s after %.3f s%s", desc(e), dt,
                          e && lsd_upcall && bundle_dir_empty() ? ", answered as success" : "");
    completion(e && lsd_upcall && bundle_dir_empty() ? nil : e);
});
```
- `bundle_dir_empty()` is true when `/private/var/containers/Bundle/Application` has no entries other than `.` and
  `..`. A containerized app may exist (Sileo-installed apps live in `/var/jb`, not here). If one does, the real error
  passes through and the seed stays honest.
- Result: `setSeedingComplete:YES`. The header gets the build and version, so the store left at the end of the boot is
  loadable.

**R3, the trimmed scan.** Replace `-[MIFileManager enumerateURLsForItemsInDirectoryAtURL:ignoreSymlinks:withBlock:]`
(0x1b11902fc; block `B20@?0@"NSURL"8C16`, i.e. `BOOL (^)(NSURL *, unsigned char)`, YES = continue):
- When `lsd_hide=1` and `[[url path] isEqualToString:@"/Applications"]`, wrap the block. If the item's
  `lastPathComponent` is in the hidden set, return YES without calling the original block. Otherwise call it.
  Every other directory passes through untouched.
- The set comes from `/redwood/etc/lsd-hide.txt` (one `Name.app` per line, `#` comments), read into a sorted C array
  at hook time. Missing file means no hiding.
- One line after the scan: `lsd: hide: %u of %u /Applications entries hidden`.
- The starting list hides 232 of ~693 bundles (33%):
  - AppleTV, Books, Bridge, Fitness, Health, HealthENBuddy, HealthENLauncher, HealthPrivacyService, Home,
    HomeCaptiveViewService, HomeControlService, HomeUIService, News, Stocks, Podcasts, Freeform;
  - FindMy, FindMyExtensionContainer, FindMyRemoteUIService, Passbook, PassbookSecureUIService, PassbookUISceneService,
    PassbookUIService, RemotePaymentPassActionsService, Family, FamilyControlsAuthenticationUI;
  - GameCenterRemoteAlert, GameCenterUIService, GameCenterWidgets, CarPlaySettings, CarPlaySplashScreen,
    CarPlayWallpaper, Sidecar, TVAccessViewService, TVSetupUIService, VideoSubscriberAccountViewService;
  - "Apple TV Remote", "Xcode Previews", "Feedback Assistant iOS", FieldTest, FTMInternal-4, DemoApp,
    StoreDemoViewService, MTLReplayer, Jellyfish, ClarityCamera, ClarityPhotos, DiagnosticsService, Diagnostics,
    DiagnosticsReporter, "Coverage Details", MBHelperApp, SequoiaTranslator, Tips;
  - Measure, Magnifier, Compass, CompassCalibrationViewService, SleepLockScreen, SleepWidgetContainer, HearingApp,
    ExposureNotificationRemoteViewService, BusinessChatViewService, BusinessExtensionsWrapper;
  - AnimojiStickers, FunCameraEmojiStickers, FunCameraShapes, FunCameraText, HashtagImages, ActivityMessagesApp,
    MobileStore, SLYahooAuth, CTCarrierSpaceAuth, "Print Center", PDUIApp, iCloud+, MusicRecognition,
    ContactlessReaderUIService, ProximityReaderUIService, CheckerBoard, CheckerBoardRemoteSetup, DisplayCal.
- Kept on purpose: Camera, Preferences, MobileSlideShow, MobileTimer, Calculator, Spotlight, InCallService,
  SharingViewService, PosterBoard, PaperBoard, Setup, PreBoard, SpringBoard's view services, AppStore, MobileSafari,
  Contacts, MobilePhone, MobileSMS, VoiceMemos, Weather, Shortcuts.
- Hiding an app only removes it from LaunchServices. Its icon disappears and it cannot be opened. No file changes.
- Toggling the list on a persisted store (R4/R4') needs a forced reseed. Say so next to the switch.

**R2, the early start.**
- `userland/mkjobs.py` (around line 94): for the labels in a new environment variable `EARLY` (build.sh default
  `com.apple.lsd`), set `job['RunAtLoad'] = True`. launchd then starts lsd when it loads the jobs (~20 s), not at the
  first Mach lookup ([47.92] today).
- Nothing else changes in the plist.

**Expected result [I, low-medium]:**
- seed ≈ 0.67 × 63 s ≈ 42 s, plus some contention, starting ~21 s → ends ~65-70 s;
- SpringBoard leaves BKSDisplayServicesStart at ~57 s and then waits only for the rest of the seed;
- UI build 13 s + first frame 4.5 s (app-launch-latency §1.1) → **first frame ~80-85 s**.
- With R4 later: ~75 s.

**What can go wrong, and the switch for it.**
- A hidden app turns out to be needed: remove its line from lsd-hide.txt, no rebuild needed beyond the image.
- The early start slows SpringBoard's own start: `EARLY=` empty.
- The upcall answer hides a real installd need: it only answers when the bundle-container directory is empty.

## 5. Question 5: one silent diagnostic run (D5) and the test plan

### 5.1 Build
- Non-BT. Tasks T1-T5 below are built in. `RC_JOBS=rc.jobs.lsd`.
- No `OSLOG` for lsd: its os_log volume and console cost would distort the timings. Use OSLOG only in a follow-up, if
  P6 fails.
- Kernel and payload rebuilt for T5 (`tools/r1-run.sh ... build`, the HANDOFF's lesson about rebuilding the payload).

### 5.2 Run
`tools/r1-run.sh d5 220 "mtkaudio=2 r1cam=4 pvr=6 pvr_gl=1 fbaspect=1 glwait=3 glthreads=1 cam_fps=10 fbscale=1 r1_clk=1 r1_cpufreq=2200 r1_gpu=680 glyuv=1 autotest=1 venc=4 mtkmic=2 proc_sample=1 lsdiag=1 lsd_upcall=1"`
- 220 s, under 230.
- The extra arguments are 36 characters; the line stays well under md3b1's ~475 overflow.
- Silent:
  - no camera and no shutter, no lock button, no sound_test, no keyboard;
  - `mtkaudio=2` stays so that the boot matches lsa2/3;
  - for a structural guarantee, drop `mtkaudio=2 mtkmic=2`; nothing measured here depends on audio.

### 5.3 `userland/rc.jobs.lsd` (each line < 400 characters; `say start` ≈ 21.5 s)
```
# D5 (docs/boot-time-lsd.md): lsd's boot seed with diagnostics, then a same-boot lsd restart that must load the store it saved. Silent: no camera, no shutter, no lock button, no sound_test.
/redwood/bin/tolog /redwood/bin/psgrep -sample 3 40 /usr/libexec/lsd &
/redwood/bin/tolog /redwood/bin/psgrep -sysctl kern.apfs_stats
/redwood/bin/rwtouch say start wait 95 say apfs1
/redwood/bin/tolog /redwood/bin/psgrep -sysctl kern.apfs_stats
/redwood/bin/rwtouch wait 20 tap 0.5 0.06 wait 1 notify com.redwood.unlock wait 10 say killlsd
/redwood/bin/tolog /redwood/bin/psgrep SpringBoard lsd
/redwood/bin/tolog /redwood/bin/psgrep -kill /usr/libexec/lsd
/redwood/bin/rwtouch wait 4 say settings notify com.redwood.settings wait 15 say end
/redwood/bin/tolog /redwood/bin/psgrep SpringBoard lsd
```

| Marker | At about | What |
|---|---|---|
| first apfs_stats | 21 s | baseline before lsd |
| apfs1 + second apfs_stats | 117 s | the seed's filesystem cost (delta) |
| unlock | 137 s | after SpringBoard's first frame (~130 s); silent |
| killlsd | 148 s | SIGTERM to lsd after ≥ 30 s idle (no dirty sentinel expected) |
| settings | 152 s | an LS client: brings lsd back on demand and is a silent launch |
| end | 167 s | run ends at 220 s |

### 5.4 Lines added (all kept; `!` prefix through `keep_line`/`kern.redwood.say`; times are uptime seconds)
- L1 `lsd: start pid %d at %.2f s, hooks %s` (constructor)
- L2 `lsd: store %.150s` (first `databaseStoreFileURLWithUID:` result)
- L3 `lsd: files at %.2f s: csstore %lld bytes mtime %ld, dirty %d error %d syncInterrupted %d sysonly %lld` (size −1 =
  absent). Printed at `createAndSeedLocalDatabase:` entry, and 5 s and 30 s after `setSeedingComplete:`.
- L4 `lsd: createAndSeed: seeded %d in %.3f s (at %.2f s)`
- L5 `lsd: builtin enumeration: ok %d in %.2f s, error %.120s`
- L6 `lsd: containerized upcall: error %.140s after %.3f s[, answered as success]`
- L7 `lsd: setSeedingComplete: %d at %.2f s`
- L8 `lsd: bundles %u in %.1f s; signing %u %.1f s max %.0f ms; data container %u %.1f s; info.plist %u %.1f s; personas %u %.1f s`
- L9 `lsd: slowest: %s %.0f ms, ...` (top 5 `entryForBundle:` by identifier, ≤ 190 characters)
- L10 `lsd: signing failed: %.60s: %.100s` (first 10)
- psgrep: `!psgrep: SIGTERM to pid %d (%s)`. The sampler also lists
  `/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*LaunchServices*` (path and bytes).
- kernel: the `swap #%u: surfaces in layers ...` line ends with ` at %llu ms`.

### 5.5 Read-out of D5

| Seen in the kept log | Means |
|---|---|
| P1: L3 at the first `createAndSeed` entry: `csstore -1` | C1 confirmed (no store on a RAM /var) |
| P2: L4 `seeded 1` at ~48-50 s | the full seed, as in lsa2/3 |
| P3: L6 with a non-empty error (expected: NSXPCConnection invalid, NSCocoaErrorDomain 4099), `answered as success` | **C2's trigger confirmed**: without the hook this error makes the seed incomplete |
| P4: L7 `setSeedingComplete: 1` | with the upcall answered, the seed completes (no other partial-result source). `0` means another source: L5/L10 name it |
| P5: L3 30 s after L7: csstore > 0 bytes, dirty 0, error 0, syncInterrupted 0; the sampler's InternalDaemon line agrees | the store is written on ramfs: C3 refuted. `-1` or no growth: C3 confirmed (look for a failed atomic move) |
| P6: after `killlsd`: L1 with a new pid, L3 the same size, **L4 `seeded 0` in < 2 s** | **in-boot proof that a complete saved store is accepted**: with a persisted /var no seed would run. `seeded 1` means a load-time check rejects it; repeat with `OSLOG=com.apple.lsd` to read "Local database thinks..." or the recovery-file line |
| P7: SpringBoard's pid the same in both psgrep lines; `settings launched by bundle id` present | restarting lsd is harmless; the Settings launch waits on nothing |
| P8: L8 categories add up to ≥ 80% of the seed's wall time; apfs_stats delta (inflate ms, lookups, lock wait ms) | which per-bundle cut is worth it (§3 table) |
| L2 path under `/private/var/mobile/Containers/Data/InternalDaemon/` | §1.1's container mapping confirmed |
| A seed wall time (L4 to L7) about lsa2's 63 s | the hooks' own cost is negligible |

### 5.6 Test plan for the fix (target: SpringBoard first frame well under 90 s). Each run is silent and uses rc.jobs.lsd without the kill line unless stated

| Run | Args / build | Pass (kept-log lines) |
|---|---|---|
| D5 | as §5.2 | P1-P7 as above |
| FT1: R1 default on + R2 | `EARLY=com.apple.lsd`, `lsdiag=1` | L1 `at` < 25 s; L4 `seeded 1`; L7 `setSeedingComplete: 1 at` < 95 s; `swap #2 ... at` < 112000 ms |
| FT2: + R3 | + `lsd_hide=1` | `lsd: hide: 82 of` (or the list's count); L7 `at` < 72 s; L8 `bundles` ≤ ~470; **`swap #2 ... at` < 88000 ms**; Settings launched; P7 |
| FT3: R4 (only once the owner allows one store write) | capture: `var_store=1`, R1 on, run ≥ 30 s past L7; then `var_store=ro` | at boot: L3 `csstore > 0`, **L4 `seeded 0` in < 2 s**; **`swap #2 ... at` < 80000 ms**; a second `ro` boot gives the same |
| Regression (once sound is allowed again) | rc.jobs.mv9 | THE PHOTO IS IN THE LIBRARY ×3, `settings launched by bundle id: 1`, as fin1 |

## 6. Verified facts vs inference, in one place

- **Verified:**
  - the store's name (4035), file layout and sibling files;
  - the startup order in `_LSServerMain`;
  - the skip condition in `createAndSeedLocalDatabase:`, and what `_LSDatabaseIsCurrent` and the seeding bit compare;
  - the upcall to installd and the partial-result path to `setSeedingComplete:NO`;
  - installd absent from our jobs;
  - the save path (`open_dprotected_np`, `replaceItemAtURL`);
  - ramfs's missing `renamex` and ignored protection class;
  - the 39-sample split; the bundle counts;
  - lsd's on-demand start at [47.92];
  - no file listing in the persist logs;
  - clients waiting on the seeding group.
- **Inferred:**
  - container class 10 = InternalDaemon (high);
  - the upcall really errors at every seed on our port (high);
  - container UUIDs survive a var_store restore (medium);
  - the gains of R2/R3/R4 (medium to low-medium);
  - per-bundle averages (assume all framework plug-ins are scanned).
- **Guessed:** whether Foundation's replace uses `RENAME_SWAP` on ramfs; the csstore size; what R5's stale container
  paths would break.

## 7. Implementation tasks (Sonnet-sized)

Rules as in HANDOFF: non-BT build per run, `tools/r1-run.sh` directly, rc.jobs lines < 400 characters. Tell the user
before any device run. No sound. Keep kept lines short (64 KB kept log).

### T1: lsd diagnostics (userland/src/inject.c; userland/build.sh:155)
- Add `lsd` to the `/redwood/lib/libredwood.dylib` INJECT line. Add `else if (strcmp(name, "lsd") == 0) lsd_hooks();`
  to the constructor dispatch (inject.c:3847).
- In a new "lsd" section (near inject.c:1990), with `lsdiag=1`, wrap with the original called first:
  - `-[_LSDefaults databaseStoreFileURLWithUID:]` → L2 once;
  - `-[LSDatabaseBuilder createAndSeedLocalDatabase:]` → L3 before, L4 after (read the `BOOL *` argument);
  - `-[ICLWorkspace enumerateBuiltInSystemContentWithBlock:error:]` → L5;
  - `-[LSDatabaseBuilder setSeedingComplete:]` → L7, then `dispatch_after` 5 s and 30 s → L3, and L8/L9 once;
  - `+[MILaunchServicesDatabaseGatherer entryForBundle:inContainer:withError:]`, the full `codeSigningInfoByValidatingResources:...error:`
    selector (§3), `-[MIExecutableBundle dataContainerCreatingIfNeeded:makeLive:created:error:]`,
    `-[MIBundle infoPlistSubset]`, `-[UMXPCServer fetchListOfPersonasSynchronousWithError:]` → count, total and maximum
    with `mach_absolute_time`. Keep the top 5 `entryForBundle:` by time with `[bundle identifier]`. Signing returning
    nil → L10 (first 10).
- L1 in `lsd_hooks()`.
- Pass: a run's kept log has L1, L2, L4, L5, L7, L8 once each per lsd instance; lsd's CPU and seed time within 5% of lsa2.

### T2: R1 upcall answer (same section)
- Replace `-[ICLWorkspace triggerRegistrationForContainerizedContentWithOptions:completion:]` as in §4.2: L6 always
  with `lsdiag=1`; answer `nil` only with `lsd_upcall=1` and an empty `/private/var/containers/Bundle/Application`.
- Pass: D5's P3 and P4.

### T3: psgrep (userland/src/psgrep.c)
- `-kill WORD`: SIGTERM to the first process whose path contains WORD; print and keep
  `!psgrep: SIGTERM to pid %d (%s)`.
- In `-sample`, also list `/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches` entries containing
  `LaunchServices` (one `opendir` of InternalDaemon, then each UUID's Caches).
- Pass: D5 has the SIGTERM line, and InternalDaemon lines appear from ~113 s on.

### T4: userland/rc.jobs.lsd
- Exactly §5.3. Pass: `!auto: start`, `apfs1`, `killlsd`, `settings`, `end` in the console; the run ends `reason 0`.

### T5: first-frame time in the kept log (xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp:1571)
- Append ` at %llu ms` (uptime from `mach_absolute_time`) to the `swap #%u: surfaces in layers` IOLog. Rebuild the
  kernel and payload.
- Pass: the kept log's `swap #2` line carries `at N ms`, with N close to the console's `[t]`.

### T6 (after D5 passes): R1 and R2 defaults (inject.c; userland/mkjobs.py ~line 94; userland/build.sh near IOS_JOBS)
- `lsd_upcall` defaults to on (`lsd_upcall=0` turns it off).
- mkjobs: `EARLY="label ..."` sets `RunAtLoad` true for those labels; build.sh exports `EARLY=${EARLY-com.apple.lsd}`.
- Pass: FT1.

### T7 (after FT1): R3 hidden bundles (inject.c; new userland/root/etc/lsd-hide.txt)
- The `enumerateURLsForItemsInDirectoryAtURL:ignoreSymlinks:withBlock:` wrapper of §4.2, gated by `lsd_hide=1`; the
  82-name list of §4.2; the `lsd: hide:` line.
- Pass: FT2. On the screenshot, hidden apps have no icon and Camera, Settings and Photos are present.

### T8 (optional, after D5's L8): persona memo (inject.c)
- With `lsd_personas=1`, the first `fetchListOfPersonasSynchronousWithError:` result is reused for 120 s inside lsd.
- Pass: L8's `personas` total drops ≥ 80%, and the seed shortens accordingly.

### T9 (needs the owner's permission for one store write): read-only golden /var (userland/src/var_store.c, userland/src/init.c:942-965)
- `var_store=ro`: init loads exactly as with `1`; `var_store watch` exits at once (no save, no claim).
- The capture is one ordinary `var_store=1` run with T2 on, ≥ 30 s past L7.
- Pass: FT3.
