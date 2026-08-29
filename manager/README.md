# The Android manager app

`manager/` holds the application a user actually touches: the superuser list, per-app root
profiles, the module list, the feature switches, boot patching, the su audit log and the
host for module WebUIs. It is a one-module Gradle build (`:app`). The Kotlin/Java namespace
is `me.weishu.kernelsu`; the shipped `applicationId` is `org.matrix.su`, or
`org.matrix.su.pr` for a pull-request build. Those two never move together: the namespace
fixes JNI symbol names, the R class and the AIDL package, while the applicationId is what
PackageManager records and what the kernel's package pin compares to.

The app is privileged in a way no other Android app is. At the instant zygote specialises a
freshly forked process into the manager's UID, the kernel installs an open file descriptor
onto its control plane into that process; nothing in the app asks for it, and it is there
before the first line of Kotlin runs. The rest of the design follows from that, and from its
mirror image: the app also ships `ksud`, the Rust userspace binary, as a native library it
can execute to obtain a root shell.

## Two channels, and what belongs on each

Traffic between the app and the rest of KernelSU splits over two channels, and that split is
not arbitrary. A *kernel decision* -- is this UID allowed root, what profile does it get, is
a feature on right now, is the device in safe mode, am I the manager -- goes through
[`ioctl()`][ioctl-2] on the kernel descriptor via the JNI bridge in
[`app/src/main/cpp`](app/src/main/cpp). *Filesystem or daemon state* -- module install and
enable, sepolicy text, profile templates, boot images, persisted feature config -- goes
through `libksud.so` run as a shell command from
[`ui/util/KsuCli.kt`](app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt). A third kind
of traffic uses neither channel: what the app pulls over the network in its own unprivileged
process. `ModuleRepoApi` reads `modules.kernelsu.org`, `TemplateRepositoryImpl` the template
index on `kernelsu.org`, `checkNewVersion()` in `Downloader.kt` the GitHub releases API, and
`DownloadService` the module zips `DownloadManager` queues. The remote-image install method
added the most involved of them, a ranged read of a boot image out of an OTA or factory
package. None of it asks the kernel anything, and what it produces reaches the privileged
side only afterwards, as a file or an argument `ksud` is handed.

Several settings sit on both sides at once. A kernel feature has a runtime value (an ioctl)
and a persisted value (a file `ksud` owns), so the su-compat switch offers three states
rather than two: enabled, disabled until reboot, disabled. The kernel-feature setters in
[`SettingsViewModel`](app/src/main/java/me/weishu/kernelsu/ui/viewmodel/SettingsViewModel.kt)
-- `setSuCompatMode`, `setKernelUmountEnabled`, `setWebViewZygoteUmountEnabled`,
`setSelinuxHideEnabled`, `setMountHideEnabled`, `setSulogEnabled` and `setAdbRootEnabled` --
write the runtime value and then call `execKsudFeatureSave()`, which runs
`ksud feature save`. Even the runtime write is not always an ioctl: su-compat,
kernel_umount, webview_zygote_umount, selinux_hide and mount_hide go through `Natives`,
while
[`SettingsRepositoryImpl`](app/src/main/java/me/weishu/kernelsu/data/repository/SettingsRepositoryImpl.kt)
implements `setSulogEnabled` and `setAdbRootEnabled` as `ksud feature set <name> <0|1>`.
The theme, skin and other preference setters touch neither channel, and
`setDefaultUmountModules` takes a third route again -- a profile ioctl with nothing
persisted after it.

The number behind each feature name is itself part of the ABI. `KSU_IOCTL_SET_FEATURE`
carries an id from `enum ksu_feature_id` in [`../uapi/feature.h`](../uapi/feature.h), and
`ksud` writes that same integer into `/data/adb/ksu/.feature_config`, so a config saved
before a renumbering switches on the wrong feature after one. Upstream allocates from zero
upward and has taken 5 for `KSU_FEATURE_WEBVIEW_ZYGOTE_UMOUNT`; this fork's
`KSU_FEATURE_MOUNT_HIDE` sits at 16, and the header reserves 16 and above for fork-local
features exactly so a rebase cannot slide one under an upstream allocation. The two are
complementary rather than alternatives, and the settings screen offers both:
webview-zygote umount strips KernelSU's mounts from uid 1053 and every process forked from
it, while mount hide filters the records `/proc` prints to any isolated reader. One
changes what is mounted, the other what is legible.

## Project layout

| Path | Contents |
| --- | --- |
| [`app/src/main/cpp/`](app/src/main/cpp) | `ksu.cc` (ioctl client) and `jni.cc` (JNI entry points), built by CMake into `libkernelsu.so`; `adbroot.cc`, built separately into `libadbroot.so`, an `LD_PRELOAD` shim for `adbd` the app only ever copies to disk; a `uapi` symlink to the repo-root headers. Both libraries link LSPosed's prefab `libcxx`, hence `ANDROID_STL=none` in [`app/build.gradle.kts`](app/build.gradle.kts) |
| [`.../java/me/weishu/kernelsu/`](app/src/main/java/me/weishu/kernelsu) | `Natives.kt`, `Kernels.kt`, `KernelSUApplication.kt`, plus `data/{model,repository}/` holding immutable models and the interfaces every ViewModel is written against |
| [`.../kernelsu/core/`](app/src/main/java/me/weishu/kernelsu/core) | `tasks/{BootKernelVersion,ExtractImage,Payload}.kt` and `utils/DataSourceChannel.kt`: the OTA and factory-image reader behind the download install method, pure Kotlin with no `ksud` and no root shell in it |
| `.../ui/screen/<name>/` | One directory per screen: `<Name>Screen.kt` host, `<Name>UiState.kt`, `<Name>Material.kt`, `<Name>Miuix.kt` |
| `.../ui/viewmodel/`, `.../ui/component/` | Eight `ViewModel`s, not one per screen: `MainActivityViewModel` plus home, superuser, module, modulerepo, settings, sulog and template. `screen/appprofile/` and `screen/colorpalette/` borrow `SuperUserViewModel` and `SettingsViewModel`; the other five screens keep their state in the `<Name>Screen.kt` host. Shared widgets, themselves split per skin |
| [`.../ui/theme/`](app/src/main/java/me/weishu/kernelsu/ui/theme) | `KernelSUTheme` and `ThemeController` in `Theme.kt`, the two colour schemes in `MaterialTheme.kt` and `MiuixTheme.kt`, and the colour and typography tokens in `Colors.kt`, `ThemeExt.kt` and `Type.kt` that both renderers draw from |
| `.../ui/webui/`, `.../magica/` | The module WebUI host; and the app-zygote late-load entry point, its isolated `MagicaService` and the disabled-by-default boot receiver that starts them |
| [`app/src/main/aidl/`](app/src/main/aidl), [`app/src/main/proto/`](app/src/main/proto), [`app/src/main/jniLibs/`](app/src/main/jniLibs) | `IKsuInterface.aidl`, interface of the root-side package-listing service; `update_metadata.proto`, AOSP's `payload.bin` schema, compiled to the protobuf-lite runtimes at build time; and a lone `.gitignore` -- `libksud.so` is never committed |

## The JNI bridge

### Getting the descriptor

The kernel's control plane is an anonymous inode -- a `struct file` with a name and a
`file_operations` table but no entry in any filesystem, so there is no path to `open()` and
no `/dev` node a policy rule could guard. The only way to hold one is for the kernel to put
it into your descriptor table. That does not make it invisible: `readlink()` on
`/proc/self/fd/N` yields `anon_inode:[ksu_driver]`, which is exactly what every scanner
below keys on. `ksu_install_fd()` in
[`kernel/supercall/supercall.c`](../kernel/supercall/supercall.c) builds it with
`anon_inode_getfile("[ksu_driver]", ...)` and publishes it with `fd_install()`; its caller
here is `ksu_handle_setresuid()` in
[`kernel/hook/setuid_hook.c`](../kernel/hook/setuid_hook.c), which fires on every successful
[`setresuid`][setresuid-2] -- and zygote always calls that with three identical UIDs:

```c
    if (unlikely(is_uid_manager(new_uid))) {
        spin_lock_irq(&current->sighand->siglock);
        ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
        ksu_set_task_tracepoint_flag(current);
        spin_unlock_irq(&current->sighand->siglock);

        pr_info("install fd for manager: %d\n", new_uid);
        ksu_install_fd();
        return 0;
    }
```

The [seccomp][seccomp-filter] poke deserves a note. A seccomp filter carries a per-syscall
constant-action bitmap consulted before the [BPF][bpf] program runs, so setting the bit for
`__NR_reboot` forces ALLOW for that syscall without touching the filter. It matters because
`ksud` gets its own descriptor by calling [`reboot(2)`][reboot-2] with a magic argument
pair, which the app filter would otherwise kill it for.

Nothing in the app uses that path. `scan_driver_fd()` in
[`app/src/main/cpp/ksu.cc`](app/src/main/cpp/ksu.cc) walks `/proc/self/fd`, `readlink()`s
every numeric entry, and takes the first whose basename contains `[ksu_driver]` -- a literal
shared with `ksud`'s own scanner and its module-unload logic, so changing it breaks three
scanners at once. Every call funnels through `ksuctl()`, a variadic template that resolves
the descriptor lazily and carries a `static_assert` capping the argument pack at one,
because no command in the table takes two.

There is no fallback here. `ksud`'s client falls back to the [`reboot`][reboot-2] magic when
its scan fails; the manager's does not. If the descriptor was never installed, `fd` stays
`-1`, every ioctl fails with `EBADF`, and the app degrades to a read-only shell rather than
crashing. `legacy_get_info()` in [`app/src/main/cpp/ksu.h`](app/src/main/cpp/ksu.h) issues
`prctl(0xDEADBEEF, 2, ...)` as a second fallback, but this fork has no [`prctl`][prctl-2]
hook under `kernel/` at all, so it always fails here and survives only for upstream kernels.

### Marshalling a profile

[`app/src/main/cpp/jni.cc`](app/src/main/cpp/jni.cc) exists because Kotlin cannot lay out
`struct app_profile`. `getAppProfile` zero-fills the struct, stamps its `version` field with
`KSU_APP_PROFILE_VER`, copies the UID in, issues the ioctl, then reflects field by field
onto `me/weishu/kernelsu/Natives$Profile` by name and signature -- `"currentUid"/I`,
`"allowSu"/Z`, `"groups"` and `"capabilities"` as `Ljava/util/List;`, `"flags"/J`, and so
on. Renaming a Kotlin property produces no compile error, only a runtime failure -- and
release builds set `isMinifyEnabled = true`, so R8 would rename or strip those fields out
from under the JNI lookups on its own. `@Keep` on `Natives.Profile` is what stops it.

Two asymmetries are load-bearing. The kernel looks profiles up by UID alone:
`do_get_app_profile()` in [`kernel/supercall/dispatch.c`](../kernel/supercall/dispatch.c)
copies in only the `profile.curr_uid` field and never reads the package name, so packages
sharing a UID share one profile -- which is why the superuser screen groups by UID. On the
way out only `capabilities.effective` is written: `capListToBits()` packs the Integer list
gating each entry on `cap_valid()`, while `permitted` and `inheritable` stay zero.

One limitation, plainly. `strcpy(p.rp_config.profile.selinux_domain, cdomain)` writes into a
64-byte field with no length test, the [SELinux][selinux] dialog in
[`RootProfileConfigMaterial.kt`](app/src/main/java/me/weishu/kernelsu/ui/component/profile/RootProfileConfigMaterial.kt)
validates the domain's shape with a regex but caps no length, and the native code is built
`-fno-stack-protector`. `profile_valid()` in the kernel rejects an over-long domain, but
only after the JNI has run off the end of `selinux_domain` into the `namespaces` and `flags`
that follow it inside `struct root_profile` -- and, for a string longer than roughly eighty
bytes, past the end of the stack-allocated `app_profile` in `setAppProfile` altogether.

### The mirroring rule

The ABI lives in one place: [`../uapi/supercall.h`](../uapi/supercall.h) and its siblings,
reached from the app through the symlink `app/src/main/cpp/uapi`; the kernel includes the
same files through `kernel/include/uapi` and `ksud` generates Rust bindings from them. Never
fork a copy. Every supercall the app needs has to be mirrored down the whole chain: a
wrapper in `ksu.cc`, a `Java_me_weishu_kernelsu_Natives_*` entry point in `jni.cc`, and an
`external` declaration in `Natives.kt` whose name matches. The mirror is by string and
resolved the first time each method is called, so a missing link is an
`UnsatisfiedLinkError` at that call site, not a build failure. The app mirrors a subset
today -- `GET_INFO`, `GET_INFO_LEGACY`, `CHECK_SAFEMODE`,
`NEW_GET_ALLOW_LIST`, `UID_SHOULD_UMOUNT`, `GET_APP_PROFILE`, `SET_APP_PROFILE`,
`GET_FEATURE`, `SET_FEATURE` -- and reaches everything else through `ksud`.

Changing the shape of any struct in `uapi/` obliges you to bump `KERNEL_SU_UAPI_VERSION`.
Most ioctl numbers in this fork encode a size field of zero, so the dispatcher cannot reject
a stale caller structurally; the version integer is the only thing that catches the drift,
and `Natives.checkUAPIMismatch()` is all that stands between a layout change and the manager
writing garbage into a kernel struct.

## Natives.kt as the typed facade

[`Natives.kt`](app/src/main/java/me/weishu/kernelsu/Natives.kt) is the single Kotlin object
that owns `System.loadLibrary("kernelsu")`, every `external` declaration, the `Profile` data
class the JNI reflects on, and the compatibility gate. One file bypasses it:
[`AppZygotePreload.java`](app/src/main/java/me/weishu/kernelsu/magica/AppZygotePreload.java)
runs inside the app zygote before any of this object is touched, so it calls
`System.loadLibrary("kernelsu")` itself and declares its own `native` method
`forkDontCareAndExecKsud`.

Two independent numbers describe compatibility, and they fail differently. `version` is
`30000 + git rev-list --count HEAD`, computed identically in
[`../kernel/Kbuild`](../kernel/Kbuild) and [`build.gradle.kts`](build.gradle.kts), so a
mismatch only means kernel and manager came from different commits. `KERNEL_SU_UAPI_VERSION`
means struct layouts changed, which is not survivable; hence `requireNewKernel()`, true when
`version` is neither `-1` nor at least `MINIMAL_SUPPORTED_KERNEL` (32513, the commit that
added the UAPI field), or whenever `checkUAPIMismatch()` is. The `-1` guard carries the
weight here: a device with no KernelSU in the kernel reports `-1`, which is numerically
below the minimum, and must not be told to update a kernel that has nothing to update.

That predicate gates real behaviour. `MainActivity.onCreate` runs the `ksud` self-install
only when `Natives.isManager && !Natives.requireNewKernel()`; `MainScreen` derives
`isFullFeatured` from it and turns off pager scrolling and the navigation badges when it is
false; `HomeUiState` turns it into `showRequireKernelWarning`, and into
`showUAPIMisMatchWarning` when the UAPI numbers also differ.

`Natives` also owns the one profile whose key is not a package name: the default non-root
profile is the pair (key `"$"`, `curr_uid` 9999) that `setDefaultUmountModules()` sends, and
`ksu_set_app_profile()` returns `-EINVAL` for UID 9999 under any other key.

## The bundled ksud, and where it is actually required

`ksud` ships inside the APK as `lib/<abi>/libksud.so`. Because
`packaging.jniLibs.useLegacyPackaging = true` the platform extracts native libraries to real
files at install time, and `nativeLibraryDir` is the one directory where an Android app can
hand the system an executable path -- which is what `getKsuDaemonPath()` returns.
`createRootShell()` builds a libsu `Shell` from `<libksud.so> debug su`, adding `-g` for a
shell that joins init's mount namespace so module mounts are visible; on failure it falls
back to the system `su`/`su -mm`, then to plain `sh`, so the app never crashes for want of
root and merely loses every `ksud` subcommand. `KsuCli` caches `SHELL` and
`GLOBAL_MNT_SHELL` for the process lifetime, while anything long-running or output-streaming
uses `withNewRootShell {}` so a hung module script cannot poison them.

Bootstrapping a system that has never seen KernelSU userspace is what `install()` does, in a
fresh root shell: `ksud install --libadbroot <nativeLibraryDir>/libadbroot.so --data-path
<deviceProtectedDataDir>`, which `MainActivity.onCreate` runs on every start that clears the
manager and kernel-version gate. On the other side
[`../userspace/ksud/src/utils.rs`](../userspace/ksud/src/utils.rs) copies `/proc/self/exe`
-- deliberately unresolved, so `/data/adb/ksud install` cannot delete the binary it is
executing -- to `/data/adb/ksud`, symlinks `/data/adb/ksu/bin/ksud`, and copies
`libadbroot.so` to `/data/adb/ksu/lib/`, the exact path
[`../kernel/feature/adb_root.c`](../kernel/feature/adb_root.c) checks before rewriting
`adbd`'s envp. Copying the running executable keeps `/data/adb/ksud` byte-identical to the
`ksud` in the installed APK, so kernel, daemon and manager cannot drift.

Now the part commonly misstated. **Gradle does not need `libksud.so`.**
`app/src/main/jniLibs/.gitignore` ignores it, nothing in `build.gradle.kts` references it,
and CI runs `./gradlew clean assembleRelease` with no `ksud` present. The hard requirements
are elsewhere: at runtime, where its absence silently costs the app root, and at repack
time, where [`../repack_apk.py`](../repack_apk.py)'s `assert_required_libs()` raises
`Missing libksud.so in APK for architecture(s): ...` before signing.

## The Compose UI, screen by screen

The UI carries a hard two-skin split. Every screen is a directory of at least four files: a
`<Name>Screen.kt` that owns all state, side effects and navigation, a `<Name>UiState.kt`
declaring the immutable state class and the bag of lambdas the host fills in, and two pure
renderers, `<Name>Material.kt` and `<Name>Miuix.kt`, chosen on `LocalUiMode` -- a
`staticCompositionLocalOf` backed by the `ui_mode` preference. Neither of those two classes
carries one name across the fourteen screens: the state is `<Name>UiState` everywhere but
`sulog`, whose is `SulogScreenState`, and the lambdas are `<Name>Actions` on eight screens
and `<Name>ScreenActions` on the other six. Miuix (a HyperOS-style component set) and
Material 3 Expressive have incompatible scaffolds, dialogs and colour-scheme types, so one
parameterised tree was not achievable; keeping all logic in the host is what stops the
renderers drifting, and shared widgets under `ui/component/` follow the same shape. The
split changes behaviour too: a host reports failures with a Material `Snackbar` and, under
Miuix, usually a `Toast` -- except `InstallScreen` and `AppProfileTemplateScreen`, which
hold a Miuix `SnackbarHostState` and show a snackbar in either skin. And `AppProfileScreen`
refreshes the superuser list after a profile change only in Material mode.

Eight of the fourteen screen directories carry a fifth file, `<Name>Utils.kt`, for pure
helpers the host needs but neither renderer should own -- `about`, `appprofile`,
`executemoduleaction`, `flash`, `home`, `install`, `module` and `templateeditor`.
`screen/modulerepo/` carries `ModuleRepoModels.kt` in place of one, and `screen/module/`
adds `ModuleShortcutState.kt` on top of its own. The single break in the file-naming rule is
`screen/colorpalette/`, whose renderers are `ColorPaletteScreenMaterial.kt` and
`ColorPaletteScreenMiuix.kt` with the `Screen` left in.

[`MainActivity`](app/src/main/java/me/weishu/kernelsu/ui/MainActivity.kt) is the only
activity in the main task. It hosts a Navigation3 `NavDisplay` whose `Route.Main` entry is a
four-page `HorizontalPager`; `Route.Home`, `Route.SuperUser`, `Route.Module` and
`Route.Settings` resolve to that same `mainScreenEntry()` rather than to anything of their
own, and every remaining route is pushed on top of it.

| Page or route | Host | What it does |
| --- | --- | --- |
| Pager 0 | `screen/home/` | Status card (working / not installed / unsupported), version and UAPI warnings, jailbreak button |
| Pager 1 | `screen/superuser/` | App list grouped by UID, granted-root and custom-profile ordering, entry to the profile editor |
| Pager 2 | `screen/module/` | Module list from `ksud module list`, enable and uninstall, update checks, WebUI and action launchers |
| Pager 3 | `screen/settings/` | Feature toggles, theme and skin selection, uninstall entry points |
| `Route.AppProfile` | `screen/appprofile/` | Root and non-root profile editing for one UID, including SELinux domain and rules |
| `Route.Install`, `Route.Flash` | `screen/install/`, `screen/flash/` | Collect boot-patch parameters, or the URL of a package to pull an image out of, then run the flash and stream its output |
| `Route.AppProfileTemplate`, `Route.TemplateEditor` | `screen/template{,editor}/` | Profile templates, local and fetched from the docs site |
| `Route.Sulog`, `Route.ModuleRepo`, `Route.ExecuteModuleAction` | `screen/{sulog,modulerepo,executemoduleaction}/` | Su audit log from `/data/adb/ksu/log`; online module browser; runs one module's action script |
| `Route.About`, `Route.ColorPalette`, `Route.ModuleRepoDetail` | `screen/{about,colorpalette,modulerepo}/` | App name, version name and the outbound source and Telegram links; the appearance editor -- colour mode, key colour, palette style and spec, blur, floating bottom bar, navigation badges, predictive back and page scale; and one repo module's detail page, rendered by `ModuleRepoDetailScreen` in the same `modulerepo` directory as the list |

The superuser list needs a root-side helper, because PackageManager in the app's own process
sees only the current Android user.
[`KsuService`](app/src/main/java/me/weishu/kernelsu/ui/KsuService.kt) is a libsu
`RootService` that reflects into `UserManager#getAliveUsers` and
`PackageManager#getInstalledPackagesAsUser`, returning a `ParcelableListSlice` because a
full `PackageInfo` list for every user exceeds the 1 MB binder limit. That listing is
expensive enough that it is cached process-wide rather than per instance: `cachedApps` and
`cachedGroupedApps` are `companion object` fields on
[`SuperUserViewModel`](app/src/main/java/me/weishu/kernelsu/ui/viewmodel/SuperUserViewModel.kt),
read and written under `appsLock` and `groupedAppsLock`. `KernelSUApplication` implements
`ViewModelStoreOwner` and builds one instance in `onCreate()` purely to call `loadAppList()`
and warm that cache; the instance `MainActivity` obtains from
`viewModel<SuperUserViewModel>()` comes from a different store, and `prepareWebView()`
constructs a throwaway one directly. All three read the same statics, which is why the list
survives Activity recreation.

`InstallScreen` gathers its options from `ksud boot-info` subqueries -- `current-kmi`,
`available-partitions`, `default-partition`, `is-ab-device`, `slot-suffix` -- and offers
four methods built from them: select a local image, download a remote one, and, only when
root is available and the kernel is GKI, direct install, joined by the inactive slot only
on an A/B device. Three of the four push `Route.Flash(FlashIt.FlashBoot(...))`, and
`FlashScreen` streams `ksud boot-patch` through a fresh root shell; the download method
pushes `FlashIt.DownloadBoot` instead and is described in the next section. Two behaviours
turn on how the kernel was loaded. Flashing a boot image while `Natives.isLateLoadMode` is
true sits behind a ten-second countdown dialog. And the reboot button after a module flash
runs `ksud soft-reboot` rather than `svc power reboot` whenever `isSoftRebootPreferred()`
holds, which it does unconditionally in late-load mode, because a real reboot drops the
jailbreak, and otherwise only when the `soft_reboot` preference the settings screen writes
is on. A soft reboot is `stop`, a replay of post-fs-data so the module mounts come back,
then `start`; the bootloader is never involved.

## Downloading and patching an image the device never had

Choosing the download method opens
[`DownloadDialog`](app/src/main/java/me/weishu/kernelsu/ui/component/dialog/DownloadDialog.kt),
whose confirm button stays disabled until the text parses as an `https` URL carrying both a
host and a path. Confirming it calls `probeRemoteBootPartitions()`, which reports which of
`boot`, `init_boot` and `vendor_boot` the package actually holds and fills the partition
dropdown from that answer; the manual KMI dialog is skipped, because this path derives the
KMI itself. `FlashScreen` then runs `downloadBoot()`, which pulls one
partition out of the package and hands the resulting file to `ksud boot-patch`. Everything
up to that hand-off happens inside the app: no `ksud`, no root, and no download of the
whole package.

An OTA or factory ZIP keeps its central directory at the end, so a server that answers
byte-range requests makes the interesting parts addressable without a download.
[`DataSourceChannel`](app/src/main/java/me/weishu/kernelsu/core/utils/DataSourceChannel.kt)
is that observation as a `SeekableByteChannel`: a `HEAD` establishes `Content-Length` and
rejects the URL outright unless the response advertises `Accept-Ranges: bytes`, and each
read becomes a ranged `GET` whose 206 body is copied into the caller's buffer. Two cache
shapes sit in front of the network: 16 KB centred on the requested offset for the scattered
small reads a ZIP directory walk makes, and 1 MB filled forward whenever the request runs
over 1 KB or starts exactly where the last cache ended. A request over 512 KB fills neither
and streams straight through, since caching it would buy nothing. `slice()` returns a view
over a byte range sharing the same client, which is what lets a `payload.bin` entry inside
the outer ZIP, or an `image-*.zip` inside a factory package, be handed to the next parser as
though it were a file of its own.

[`ExtractImage`](app/src/main/java/me/weishu/kernelsu/core/tasks/ExtractImage.kt) decides
which shape it is looking at. A `payload.bin` entry means an A/B OTA; a `boot.img`,
`init_boot.img` or `vendor_boot.img` entry means a factory image, whose images may sit one
level deeper inside `image-*.zip`. Both nested forms insist the enclosing entry be
`STORED` and not `DEFLATED`, since a compressed container cannot be sliced into.
[`Payload`](app/src/main/java/me/weishu/kernelsu/core/tasks/Payload.kt) reads the `CrAU`
header, refuses any format version but 2, and parses the `DeltaArchiveManifest` generated
from [`app/src/main/proto/update_metadata.proto`](app/src/main/proto) -- AOSP's own schema,
compiled by the protobuf Gradle plugin into the Java and Kotlin lite runtimes, with
`-shrinkunusedprotofields` in [`app/proguard-rules.pro`](app/proguard-rules.pro) so R8
drops the descriptor fields nothing reads. Extraction replays the partition's
`InstallOperation` list into the output file and, when the manifest carries a
`new_partition_info.hash`, compares the SHA-256 it accumulated against it, raising
`IOException` on a mismatch rather than patching a corrupt image.

One value has to be settled before `ksud` is invoked and cannot be read off the device: the
KMI, the `android<N>-<major>.<minor>` string that decides which prebuilt LKM belongs in the
image. A downloaded image is unrelated to the running kernel, so letting `ksud` fall back
to the local KMI would inject the wrong module, and `init_boot` and `vendor_boot` carry a
ramdisk with no kernel in it, so the answer often is not in the file that was extracted.
[`BootKernelVersion`](app/src/main/java/me/weishu/kernelsu/core/tasks/BootKernelVersion.kt)
mirrors ksud's `parse_kmi_from_boot` for this: read the `ANDROID!` header, take the kernel
block offset and size from the layout for that header version, decompress a gzip, XZ or LZ4
prefix when the block is compressed, and match `(\d+\.\d+)(?:\S+)?(android\d+)` against
the Linux banner, growing the probe window until it hits or the block runs out. `Payload`
applies the same scan to the payload's own `boot` or `vendor_kernel_boot` partition, trying
the middle operation before the sequential walk because the banner in an EFI-packaged GKI
image sits roughly halfway into the kernel. Whatever comes back is appended to the
`boot-patch` command line as an explicit `--kmi`; when the user supplied neither an LKM nor
a KMI by hand and the scan finds nothing, `downloadBoot()` fails the flash rather than
guessing. It also runs that command on libsu's main shell, `Shell.getShell()`, rather than
on the fresh one `flashWithIO()` opens for the local and direct install paths. That is the
only call in `KsuCli.kt` reaching neither `getRootShell()` nor `withNewRootShell {}`, so it
is the only one whose shell was not built by `createRootShell()` from
`<libksud.so> debug su`.

## The WebUI host

A module can ship a web front end under `/data/adb/modules/<id>/webroot`, a path no
unprivileged app can read.
[`WebUIActivity`](app/src/main/java/me/weishu/kernelsu/ui/webui/WebUIActivity.kt) is not
exported and takes only an `id` query parameter. `prepareWebView()` in
[`WebViewHelper.kt`](app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewHelper.kt) re-lists
modules through `ksud` and refuses anything not installed, shipping no `webroot`, not
enabled, or carrying a pending update or removal mark, so a stale shortcut cannot
resurrect a module the user turned off. It then opens a dedicated global-mount root shell
and builds a WebView with `javaScriptEnabled` and `domStorageEnabled` true and
`allowFileAccess` false.

Content is served over a virtual origin, `https://mui.kernelsu.org/`, by a
`WebViewAssetLoader` whose single path handler is
[`SuFilePathHandler`](app/src/main/java/me/weishu/kernelsu/ui/webui/SuFilePathHandler.java),
which reads each file through libsu's `SuFile`/`SuFileInputStream` bound to that root shell.
A fake `https` origin rather than `file://` is what lets `allowFileAccess` stay off while
pages keep a normal same-origin story for `fetch` and `localStorage`. Two synthetic paths
are answered before disk is touched: `internal/insets.css` returns the safe-area geometry,
and `internal/colors.css` the manager's palette, rendered by
[`MonetColorsProvider.kt`](app/src/main/java/me/weishu/kernelsu/ui/webui/MonetColorsProvider.kt)
and returned as an empty stylesheet unless the `color_mode` preference is 3 to 6 or the
skin is Material, so a page needs its own fallback colours. `getCanonicalFileIfChild()` is
the only guard against path traversal on a root-privileged read.

The JS bridge,
[`WebViewInterface`](app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewInterface.kt), is
registered as `ksu` and exposes `exec` in three overloads (synchronous, callback, and
callback with `cwd`/`env` options), `spawn` emitting `stdout`/`stderr`/`exit`/`error`
events, plus `toast`, `fullScreen`, `enableEdgeToEdge`, `moduleInfo`, `listPackages`,
`getPackagesInfo` and `exit`, each `exec` and `spawn` opening a fresh global-mount root
shell. Say this plainly: any page under an enabled module's webroot runs arbitrary commands
as root, with no allowlist and no per-module capability model. See
[`website/docs/guide/module-webui.md`](../website/docs/guide/module-webui.md) for the author
view.

`MainActivity` is exported and accepts `ksu://action` and `ksu://webui` VIEW intents, so any
app on the device could otherwise fire a deep link that runs a module's root action script.
[`IntentDispatcher`](app/src/main/java/me/weishu/kernelsu/ui/navigation3/IntentDispatcher.kt)
requires a token -- 32 `SecureRandom` bytes minted once per install and kept in
`SharedPreferences` -- on every `ksu://` URI, and `Shortcut.buildShortcutUri()` embeds it in
pinned shortcuts. Drop either half and `ksu://action` becomes an unauthenticated
remote-execution entry point.

## The app-zygote jailbreak path

[`AndroidManifest.xml`](app/src/main/AndroidManifest.xml) declares
`android:zygotePreloadName` on `<application>` and an `isolatedProcess`
[`MagicaService`](app/src/main/java/me/weishu/kernelsu/magica/MagicaService.java) with
`android:useAppZygote="true"`, so starting that service makes the platform spin up the app's
own zygote and call
[`AppZygotePreload.doPreload()`](app/src/main/java/me/weishu/kernelsu/magica/AppZygotePreload.java)
while the process is still privileged -- the only moment in a normal app process tree where
`setuid(0)` succeeds. `fork_dont_care_and_exec_ksud()` in `jni.cc` double-forks so the
grandchild is reparented onto init, then execs
`ksud late-load --magica 5555 --package-name <pkg>`. On success `ksud` force-stops and
restarts the manager, which is why the jailbreak button gives up after a thirty-second
timeout rather than waiting for a result.

That whole sequence can also run with nobody pressing anything.
[`BootCompletedReceiver.java`](app/src/main/java/me/weishu/kernelsu/magica/BootCompletedReceiver.java)
ships disabled -- `android:enabled="false"`, in its own `:magica_boot` process -- and
listens for `LOCKED_BOOT_COMPLETED`, `BOOT_COMPLETED` and
`me.weishu.kernelsu.magica.LAUNCH`. It is marked `android:directBootAware="true"`, which is
what lets `LOCKED_BOOT_COMPLETED` reach it at all: before the user unlocks,
credential-encrypted storage is unavailable and only direct-boot-aware components run, so
that broadcast is the earliest point at which the jailbreak can be attempted. The receiver
returns immediately if `rootAvailable()` already holds, and otherwise starts
`MagicaService`, which spins up the app zygote and repeats everything above.

The auto-jailbreak switch is therefore not a flag the receiver reads but a component-enabled
state: `SettingsRepositoryImpl.autoJailbreak` calls `setComponentEnabledSetting` with
`DONT_KILL_APP`, and mirrors the choice into the `auto_jailbreak` preference only so the
settings screen can show it back.

## Proving identity to the kernel

The kernel cannot ask PackageManager who the manager is -- PackageManager is not running at
the times this matters, and would be an untrusted answer anyway. Instead
[`kernel/manager/apk_sign.c`](../kernel/manager/apk_sign.c) parses each candidate `base.apk`
by hand: it locates the ZIP end-of-central-directory record, walks to the APK Signing Block,
finds the v2 block (id `0x7109871a`), and compares the length and SHA-256 of the DER signing
certificate against constants baked in by [`../kernel/Kbuild`](../kernel/Kbuild)
(`KSU_EXPECTED_SIZE` / `KSU_EXPECTED_HASH`, plus an optional second slot). It refuses the
APK if a ZIP64 end-of-central-directory locator precedes the record, if the number of v2
blocks in the signing block is anything but exactly one, or if a v3 (`0xf05368c0`) or v3.1
(`0x1b93ad61`) block is present.

Those rejections are the interesting half. Android verifies with the highest scheme present,
so an attacker who bolted a v3 signature onto an APK carrying the genuine -- and public --
v2 certificate would run under a different key from the one the kernel hashed. Seeing that
v3 block therefore has to be guaranteed, and until recently it was not: the parser gave up
after ten length-prefixed pairs, so an APK could lead with the official v2 block, pad with
nine junk pairs, and park its real v3 signature past the tenth. The kernel never reached
that block and crowned the APK as manager; the platform did reach it and ran the APK under
the attacker's key (CAN-2026-2035133). The loop now walks every pair to the end, and the
ZIP64 test in front of it rules out the archive shape whose 32-bit end-of-central-directory
fields are placeholders, so the kernel cannot be steered at a different signing block from
the one the platform parses.

What the kernel no longer does is scan the local file headers for `META-INF/MANIFEST.MF`.
The platform ignores a v1 signature whenever a v2 one is present, and an APK whose v2
signature fails to verify never installs at all, so a v1 signature sitting beside a
verified v2 signature proves nothing and costs a walk of every entry in the archive to
find. Signing the manager v2-only is still the right thing to do, and
[`../repack_apk.py`](../repack_apk.py) does it with `--v1-signing-enabled false`,
`--v2-signing-enabled true` and both v3 flags false.

[`kernel/manager/throne_tracker.c`](../kernel/manager/throne_tracker.c) resolves the winning
package name to a UID by parsing `/data/system/packages.list`, and
[`kernel/manager/pkg_observer.c`](../kernel/manager/pkg_observer.c) re-runs it from an
fsnotify mark, so a freshly installed manager is crowned without a reboot. Identity is then
`ksu_manager_appid == uid % 100000`, the modulo letting a manager installed for a secondary
Android user still count, and [`kernel/supercall/perm.c`](../kernel/supercall/perm.c) turns
that into the permission model: `only_manager()` gates `GET_APP_PROFILE` and
`SET_APP_PROFILE`, `manager_or_root()` the allowlist reads and the feature toggles.

The consequence for a fork is direct. Re-signing the manager with a different key requires
rebuilding the kernel module with the matching size and hash, or the app never receives its
descriptor at all. If the kernel was also built with `KSU_MANAGER_PACKAGE`, the comparison
is exact string equality against the `applicationId`, so a kernel pinned to `org.matrix.su`
rejects the PR build's `org.matrix.su.pr` whichever certificate slot matches.

## Build, sign and rename

Start with the local build, which is what the [`../justfile`](../justfile) encodes. `ksud`
is a member of the repo-root Cargo workspace, so its output lands in `<repo>/target/`, not
under `userspace/`:

```sh
cross build --target aarch64-linux-android --release
cp target/aarch64-linux-android/release/ksud \
   manager/app/src/main/jniLibs/arm64-v8a/libksud.so
cd manager && ./gradlew aDebug
```

For a release build, signing comes from the LSPosed `apksign` plugin, which reads four
Gradle properties -- `KEYSTORE_FILE`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`; see
[`sign.example.properties`](sign.example.properties). The APK that `./gradlew clean
assembleRelease` produces carries `libkernelsu.so` and `libadbroot.so` for both `arm64-v8a`
and `x86_64`, and no `libksud.so` at all. Injecting it, filtering `lib/` down to the ABIs
you name, aligning to 16 KB and re-signing v2-only is a separate step:

```sh
python3 repack_apk.py repack -b release -t release -a arm64-v8a \
    -K key.jks -A "$KEY_ALIAS" -P "$KEYSTORE_PASSWORD" -S "$KEY_PASSWORD" \
    -n "MyApk-arm64" --strip
```

`repack_apk.py` takes the newest APK under `manager/app/build/outputs/apk/<type>/` and the
`ksud` at `target/<triple>/<type>/ksud`;
[`../.github/workflows/build-manager.yml`](../.github/workflows/build-manager.yml) stages
downloaded artifacts into exactly those paths, then runs it once with both ABIs on the
command line -- `-a arm64-v8a -a x86_64` -- so a single APK comes out holding both, with
`fetch-depth: 0` because the version code comes from the commit count.

Renaming is three Gradle properties, read in [`app/build.gradle.kts`](app/build.gradle.kts):

| Property | Effect |
| --- | --- |
| `-PKSU_PACKAGE_NAME=<id>` | Overrides `applicationId` (default `org.matrix.su`) |
| `-PKSU_NAME=<name>` | Overrides the `app_name` string resource (default `Matrix`) |
| `-PIS_PR_BUILD=true` | Switches both defaults to `org.matrix.su.pr` / `Matrix PR` and sets `BuildConfig.IS_PR_BUILD` |

`archivesName` follows from the manager name, the version code, and the version name that
`git describe --tags --always` produces. `BuildConfig.IS_PR_BUILD` drives the manager-side
PR warning on the home screen; the kernel-side warning is separate and comes from
`KSU_GET_INFO_FLAG_PR_BUILD`, set when the kernel was built with a second accepted
certificate slot. The rest of the toolchain is pinned in
[`gradle/libs.versions.toml`](gradle/libs.versions.toml) and
[`build.gradle.kts`](build.gradle.kts): AGP 9.3.2, Kotlin 2.4.10, NDK 29.0.14206865,
compileSdk and targetSdk 37, minSdk 31, Java 21, libsu 6.0.0, Miuix 0.9.3, and the three
libraries the remote-image reader needs -- protobuf 4.36.0 in its Kotlin lite flavour,
commons-compress 1.28.0 and xz 1.12. The protobuf Gradle plugin is what turns
`update_metadata.proto` into classes, so `settings.gradle.kts` also lists
`gradlePluginPortal()` among its plugin repositories.

## See also

- [`../docs/architecture.md`](../docs/architecture.md) -- how the layers fit together
- [`../uapi/README.md`](../uapi/README.md) -- the ABI the JNI bridge compiles against
- [`../userspace/ksud/README.md`](../userspace/ksud/README.md) -- every subcommand `KsuCli.kt` runs
- [`../kernel/manager/README.md`](../kernel/manager/README.md) -- the signature check and throne tracker
- [`../kernel/supercall/README.md`](../kernel/supercall/README.md) -- the ioctl table and its perm checks
- [`../kernel/policy/README.md`](../kernel/policy/README.md) -- what an app profile means to the kernel
- [`../website/README.md`](../website/README.md) -- the docs site, which serves the template index

<!-- reference links: kernel documentation and man pages -->
[bpf]: https://docs.kernel.org/bpf/index.html
[ioctl-2]: https://man7.org/linux/man-pages/man2/ioctl.2.html
[prctl-2]: https://man7.org/linux/man-pages/man2/prctl.2.html
[reboot-2]: https://man7.org/linux/man-pages/man2/reboot.2.html
[seccomp-filter]: https://docs.kernel.org/userspace-api/seccomp_filter.html
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
