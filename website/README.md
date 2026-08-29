# The documentation site

`website/` holds the VitePress sources for kernelsu.org: fifteen guide pages, seven
translations of them, and a `public/` tree the build copies out verbatim. Nothing built
here ends up in the kernel module or in `ksud`, and apart from this file no commit in the
fork has changed anything under the directory -- every diff it has taken since the last
rebase came from upstream, and all of it landed in `package.json` and `yarn.lock`. What
the directory does define is the format of `docs/public/templates/`, the App Profile
presets the Android manager reads over HTTP from upstream's deployment.

## Running and building it

Development is three yarn scripts. [`yarn.lock`](yarn.lock) is the committed lockfile and
the deploy job installs with `--frozen-lockfile`, which aborts if `yarn.lock` and
`package.json` disagree.

```sh
cd website
yarn install
yarn docs:dev      # dev server with hot reload
yarn docs:build    # static build into docs/.vitepress/dist
yarn docs:preview  # serve the built output
```

Those three scripts live in [`package.json`](package.json) and are thin
`vitepress <cmd> docs` wrappers, so `docs/` is VitePress's source root. Paths in this file
stay relative to `website/`.

Two dev dependencies is the whole manifest: `vitepress` at `^1.6.4` and `vue` at
`^3.5.41`, which [`yarn.lock`](yarn.lock) resolves to 1.6.4 and 3.5.41 respectively.
There is no production dependency section and no plugin, so the site is stock VitePress 1.x
with a locale table and one `buildEnd` hook. Both version ranges are carets, which means
the manifest never decides a build on its own -- the lockfile does, and Dependabot's
`npm group` rewrites the lockfile whenever anything transitive moves. Three such commits
landed in the last upstream range, and their net effect on `package.json` was a single line
(`vue` from `^3.5.40` to `^3.5.41`) against 787 changed lines of `yarn.lock`. Rebasing the
fork therefore conflicts here more often than anywhere else under `website/`, and because
no fork commit has an opinion about dependencies, the resolution is always to take
upstream's `yarn.lock` whole rather than merge hunks: a hand-stitched lockfile is exactly
what `--frozen-lockfile` exists to reject.

[`AGENTS.md`](../AGENTS.md) tells you to use bun for this directory. CI does not: it runs
`yarn install --frozen-lockfile` against `yarn.lock`. That check compares the lockfile
against `package.json` and nothing else, and nothing in the tree pins the package manager
-- there is no `packageManager` field and no `.yarnrc` -- so a `bun.lock` committed beside
an untouched `yarn.lock` goes through unremarked and the deployed site is built from
whatever `yarn.lock` still says. A dependency change made with anything but yarn has to be
mirrored into that lockfile by hand.

## Layout

| Path | What it is |
| --- | --- |
| [`docs/index.md`](docs/index.md) | English home page, VitePress `layout: home` front matter |
| `docs/<locale>/index.md` | seven per-locale home pages, same `layout: home` front matter |
| [`docs/guide/`](docs/guide/) | the fifteen English guide pages |
| `docs/<locale>/guide/` | one directory per translation: `zh_CN`, `zh_TW`, `ja_JP`, `vi_VN`, `id_ID`, `ru_RU`, `pt_BR` |
| [`docs/.vitepress/config.ts`](docs/.vitepress/config.ts) | site title, AdSense head tag, sitemap host, and the `buildEnd` hook |
| [`docs/.vitepress/locales/index.ts`](docs/.vitepress/locales/index.ts) | registers the eight locales and their display labels |
| [`docs/.vitepress/locales/en.ts`](docs/.vitepress/locales/en.ts) | English nav, sidebar, footer and edit link |
| [`docs/repos.json`](docs/repos.json) | device table data, `import`ed by a page rather than fetched |
| [`docs/public/`](docs/public/) | copied verbatim into the output: `logo.png`, `favicon.ico`, `ads.txt`, `templates/` |

The build writes `docs/.vitepress/dist`, which is the directory the Pages workflow
uploads. [`.gitignore`](.gitignore) already excludes `dist` and `docs/.vitepress/cache`,
so a local build leaves nothing to clean up before committing.

The home pages sit outside all of that. Each `index.md` is front matter rather than prose
-- `layout: home`, a page `title`, a hero name, tagline and image, two action buttons and
a `features` list of four cards -- and no shared template stands behind them, so the site
title, the landing copy, both buttons and all four cards exist in eight independent
copies. Retitling the site or repointing a button means editing all eight; adding a locale
means writing a ninth by hand.

## Adding a page and putting it in the sidebar

A page is a file plus a sidebar entry, and the two are registered independently. A
markdown file with no sidebar entry still builds and is still reachable by URL; it just
never appears in the navigation, which is how several translations have quietly lost
pages (see below).

1. Create `docs/guide/<name>.md` starting with a single H1.
2. Add `{ text: '<Title>', link: '/guide/<name>' }` to the `items` array of the single
   `{ text: 'Guide', items: [...] }` group that `sidebarGuide()` returns in
   [`docs/.vitepress/locales/en.ts`](docs/.vitepress/locales/en.ts). Push it onto the
   outer array instead and you get a second, empty sidebar group.
3. Copy the file to `docs/<locale>/guide/<name>.md` for each of the seven translations
   and add the same entry, with the locale prefix, to that locale's `sidebarGuide()`.

The sidebar is a map from URL prefix to entry list. English uses the key `'/guide/'` and
each translation uses `'/<locale>/guide/'` -- for instance
[`docs/.vitepress/locales/zh_CN.ts`](docs/.vitepress/locales/zh_CN.ts) keys on
`'/zh_CN/guide/'` and every one of its links begins with `/zh_CN/`. Locale keys use an
underscore rather than the more usual hyphen, and the directory name under `docs/` must
match the key exactly, so a page under `docs/zh_CN/` is served at `/zh_CN/guide/<name>`.
Link targets in the existing sidebars mix bare names and `.md` suffixes; both resolve.

Two quirks in the locale files are worth knowing before you copy one as a template. Each
begins with a `createRequire` and `const pkg = require('vitepress/package.json')` that
nothing subsequently reads. Each also sets `themeConfig.lastUpdatedText`, but the
top-level `lastUpdated` option is never enabled in `config.ts`, so no timestamp is
rendered -- the `fetch-depth: 0` in the deploy workflow is not doing anything for that
feature.

## Translations

All eight locales carry all fifteen guide files; the file sets under `docs/guide/` and
`docs/<locale>/guide/` are identical. The sidebars are where they have drifted apart, and
four of them under-report what is actually published:

| Locale | Sidebar entries | Pages present but unlisted |
| --- | --- | --- |
| `en`, `zh_CN`, `zh_TW`, `pt_BR` | 15 | none |
| `ru_RU` | 13 | `difference-with-magisk`, `module-webui` |
| `ja_JP` | 12 | `app-profile`, `difference-with-magisk`, `module-webui` |
| `id_ID` | 11 | the three above plus `hidden-features` |
| `vi_VN` | 9 | the four above plus `module`, `rescue-from-bootloop` |

Every locale's `socialLinks` and `editLink.pattern` point at `tiann/KernelSU`, so the
"Edit this page on GitHub" button sends a reader to upstream, not to this fork. That is
eight locale files, plus the `View on GitHub` hero action in all eight `index.md` home
pages.

[`docs/repos.json`](docs/repos.json) is consumed differently from the rest. All eight
copies of `unofficially-support-devices.md` reach it with a `<script setup>` `import`
(`../repos.json` from the English page, `../../repos.json` from a translation), so the
table is compiled into each page and there is no per-locale copy of the data.

## The template endpoint

`docs/public/` is copied into the build output untouched, so
[`docs/public/templates/`](docs/public/templates/) is served at
`https://kernelsu.org/templates/`. A static host cannot enumerate a directory, so the
`buildEnd(config)` hook in [`docs/.vitepress/config.ts`](docs/.vitepress/config.ts) reads
`<outDir>/templates`, drops dotfiles, and writes the remaining names as a JSON array to
`<outDir>/templates/index.json`. That file belongs to the build output, which
[`.gitignore`](.gitignore) excludes along with `docs/.vitepress/cache`, so it never gets
committed by accident. Writing one by hand into `docs/public/templates/` is the mistake to
avoid: the public tree is copied out before `buildEnd` runs, so the `readdir` would list
`index.json` among the template names and the write would then overwrite it. Adding a
template means adding one file to the directory and nothing else.

The manager's
[`TemplateRepositoryImpl.kt`](../manager/app/src/main/java/me/weishu/kernelsu/data/repository/TemplateRepositoryImpl.kt)
fetches `https://kernelsu.org/templates/index.json`, then one request per name against
`https://kernelsu.org/templates/%s`, validating each with `TemplateInfo.fromJSON` before
storing it.

The host, though, is upstream's. `kernelsu.org` is served from `tiann/KernelSU`'s Pages
deployment, this tree carries no `CNAME`, and the deploy job here never runs on `dev`, so
editing `docs/public/templates/` in this fork changes nothing an installed manager sees --
including a manager built from [`manager/`](../manager), which still points at
`kernelsu.org`. The directory defines the format; upstream publishes the data. The fetch
is conditional too: `getTemplates()` calls `fetchRemoteTemplates()` only when the local
template list is empty or the caller asked for a sync, so a device that already has
templates stored does not re-read the endpoint on every launch.

Each file is an App Profile preset, and
[`docs/public/templates/adaway.root`](docs/public/templates/adaway.root) shows the full
shape: `id`, `name`, `author`, `description`, `uid`, `gid`, `groups`, and optionally
`capabilities`, `context`, `namespace`, `flags`, plus a `locales` map of translated
name/description pairs. There are eleven templates today.

## Deployment

[`.github/workflows/deploy-website.yml`](../.github/workflows/deploy-website.yml) builds
and publishes to GitHub Pages. It runs `yarn install --frozen-lockfile`, then
`yarn docs:build`, then `touch docs/.vitepress/dist/.nojekyll`, the marker that stops
Pages from treating a tree as Jekyll source and dropping every path that starts with an
underscore. It is belt-and-braces on this workflow, which uploads an artifact rather than
publishing a branch: `actions/upload-pages-artifact` takes `website/docs/.vitepress/dist`
and `actions/deploy-pages` serves that tarball directly, with no Jekyll step on the path.
The `concurrency` block uses
`group: pages, cancel-in-progress: false`: cancelling a half-finished Pages deployment
can leave the live site broken, so queued runs wait instead of pre-empting.

The trigger is a push to `main` or `website`, path-filtered to `website/**` and the
workflow file itself. This fork's default branch is `dev`, so a website change merged to
`dev` never deploys -- push it to `website` or dispatch the workflow manually.

## Which guide pages describe this fork

The guide is a near-verbatim copy of upstream's, and the tree beneath it has moved out from
under several pages. Some of that is the fork replacing or feature-gating a mechanism the
guide documents; the rest is upstream changing the code without changing the page, which
happens on every rebase and is the more common of the two. Verified page by page against
the tree:

| Page | Verdict |
| --- | --- |
| [`x86_64-support.md`](docs/guide/x86_64-support.md) | Accurate. `KSU_X86_PATCH_SYSCALL_DISPATCHER` exists in [`kernel/Kconfig`](../kernel/Kconfig) with `depends on KSU && X86_64`, default n. |
| [`hidden-features.md`](docs/guide/hidden-features.md) | Accurate, and now a page short of the code. `KSURC_PATH` in [`defs.rs`](../userspace/ksud/src/defs.rs) is `/data/adb/ksu/.ksurc`, and [`su.rs`](../userspace/ksud/src/su.rs) sets `ENV` to it when the file exists and `ENV` is unset; the `su -Z` option upstream has since added is undocumented here. See below. |
| [`rescue-from-bootloop.md`](docs/guide/rescue-from-bootloop.md) | Accurate. `/data/adb/ksud`, `/metadata/ksu/modules.rc` and `/metadata/watchdog/ksu/modules.rc` all match `defs.rs`. |
| [`module-webui.md`](docs/guide/module-webui.md) | Accurate. `webroot/index.html` is the contract the manager's WebView resolves. |
| [`module.md`](docs/guide/module.md) | Accurate for this tree, apart from its `meta-overlayfs` references; see below. |
| [`difference-with-magisk.md`](docs/guide/difference-with-magisk.md) | Accurate: `/data/adb/ksu/bin/busybox`, the `mknod` deletion convention, `REMOVE`/`REPLACE`, and the `post-mount` and `boot-completed` stages. |
| [`faq.md`](docs/guide/faq.md) | One stale claim. "KernelSU modifies the `kernel`, while Magisk modifies the `ramdisk`, allowing both to work together" holds only for a built-in build -- `CONFIG_KSU` is still a `tristate` in [`kernel/Kconfig`](../kernel/Kconfig). In LKM mode, which is what `ksud boot-patch` produces, [`ksuinit/src/init.rs`](../userspace/ksuinit/src/init.rs) is written into the boot ramdisk as `/init` with the stock init displaced to `/init.real`, so both root solutions want the same file. |
| [`metamodule.md`](docs/guide/metamodule.md) | Split. The ksud-facing contract holds -- `metamount.sh`, `metainstall.sh`, `metauninstall.sh` and `/data/adb/metamodule` match `defs.rs`, and `metamodule=1` or `true` matches `is_metamodule()`. The "Real-World Example" section quotes `meta-overlayfs/src/mount.rs` as though it were in-repo; that crate is not in this tree. |
| [`module-config.md`](docs/guide/module-config.md) | Mostly right, wrong twice; see below. |
| [`app-profile.md`](docs/guide/app-profile.md) | One stale claim about kernel-side unmounting, and two features it has never heard of; see below. |
| [`installation.md`](docs/guide/installation.md) | Its pasted `ksud boot-patch -h` block no longer matches the binary, and it has no entry for the second patcher, `ksud boot-patch-v2`; see below. |
| [`how-to-build.md`](docs/guide/how-to-build.md) | Archival. Its KernelSU step curls `https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh` -- upstream's copy of [`kernel/setup.sh`](../kernel/setup.sh), which clones `tiann/KernelSU`. Following it integrates upstream, not this fork. |
| [`how-to-integrate-for-non-gki.md`](docs/guide/how-to-integrate-for-non-gki.md) | Archival, and the hook layout it tells you to patch in does not exist here; see below. |
| [`unofficially-support-devices.md`](docs/guide/unofficially-support-devices.md) | Archival, and data-only -- it renders `repos.json` and makes no claim about the code. |
| [`what-is-kernelsu.md`](docs/guide/what-is-kernelsu.md) | One dead link. It points at `https://github.com/tiann/KernelSU/tree/main/userspace/meta-overlayfs`, a directory upstream itself deleted in `3e8b4a7e` ("meta-overlayfs: Moved to module repo"). The crate is in neither tree. |

Six of those verdicts need the code behind them.

[`hidden-features.md`](docs/guide/hidden-features.md) documents exactly one thing, and that
one thing is exact. What it now misses is `su -Z`. `root_shell()` in
[`su.rs`](../userspace/ksud/src/su.rs) registers `opts.optopt("Z", "context", ...)` and,
after the identity switch, writes the value to `/proc/thread-self/attr/current` -- the
procfs interface for a dynamic [SELinux][selinux] transition, so the shell you get runs in
the context you named rather than in whatever label `su` inherited. Two upstream changes
tightened the identity switch around it. The one that added `-Z` also made `set_identity()`
return `Result` instead of swallowing every failure with `.ok()`; a follow-up then gated the
call on `identity_requested`, so it fires only when the command line actually asked for
something -- a user argument, a `-g`, or supplementary groups. That closes a quiet failure
mode: a [`setresuid`][setresuid-2] that failed used to leave the process at the uid it
already had, which for a root shell dropping to an app uid meant handing the caller root and
exec'ing anyway.

[`module.md`](docs/guide/module.md) documents the `initrc/` directory,
`/data/adb/initrc.d/`, `ksud initrc refresh`, late-load mode, `module.prop`'s `actionIcon`
and `webuiIcon`, and the `KSU_UAPI_VER`, `KSU_RUNTIME_MODE` and `KSU_LATE_LOAD` variables
that [`module.rs`](../userspace/ksud/src/module.rs) puts into a module script's
environment. All of it matches this tree, and none of it is a fork addition -- the commits
that introduced it are upstream's. Read the page as current documentation rather than as a
fork addendum. Its `meta-overlayfs` references are the only part that points outside.

[`module-config.md`](docs/guide/module-config.md) lists two supported features, but
[`uapi/feature.h`](../uapi/feature.h) defines seven -- `su_compat` 0, `kernel_umount` 1,
`sulog` 2, `adb_root` 3, `selinux_hide` 4, `webview_zygote_umount` 5 and `mount_hide` 16 --
and `parse_feature_id` in [`feature.rs`](../userspace/ksud/src/feature.rs) accepts every one
of them by name or by number, so `ksud feature set 16 1` and
`ksud feature set mount_hide 1` are the same call.

The jump from 5 to 16 is not a gap left by accident. A feature id is a wire value in two
directions at once: the manager passes it across the IOCTL boundary, and
`save_binary_config()` writes it verbatim into `/data/adb/ksu/.feature_config`, a flat
`(u32 id, u64 value)` table with no names in it. Renumber a feature and every device that
already stored a setting silently applies it to whatever now owns the old slot.
`mount_hide` was id 5 in this fork until upstream took that slot for
`webview_zygote_umount`, so it moved to 16, and the comment now standing above it in
`feature.h` reserves 16 and up for fork-local features -- upstream allocates upwards from
zero, so the two allocators cannot collide again. `FeatureId`, `from_u32`, `name`,
`description`, `parse_feature_id`, `list_features` and `save_config` in `feature.rs` all
carry both variants; adding a fork feature means touching every one of them plus
`feature.h`, and any page that names ids has to follow.

The page also claims that a falsey `manage.<feature>` value marks the feature
managed and disabled. It does not: `module.rs` runs those values through
`parse_bool_config` in [`module_config.rs`](../userspace/ksud/src/module_config.rs), which
trims the value and returns true only for `1` or a case-insensitive `true`, dropping the
key otherwise, so `manage.su_compat false` is indistinguishable from never writing the key
at all.

[`app-profile.md`](docs/guide/app-profile.md) says that on 5.10 and newer the kernel
unloads modules for an unprivileged app without any further action. That was true of the
KernelSU the page was written for; it is true of neither this tree nor current upstream.
`ksu_handle_umount()` in
[`kernel/feature/kernel_umount.c`](../kernel/feature/kernel_umount.c) returns early unless
both `ksu_module_mounted` and `ksu_kernel_umount_enabled` are set, puts the new uid through
`ksu_uid_should_umount()`, and then refuses to act at all unless `is_zygote(current_cred())`
holds, because a root app that `setuid`s to an app uid while still in the global mount
namespace would otherwise trigger a system-wide detach. What survives all that is only the
paths userspace registered through `KSU_IOCTL_ADD_TRY_UMOUNT`: `mount_list` starts empty on
every boot and the kernel discovers nothing on its own.

Two features have since been layered onto that path and the page names neither. Upstream's
`webview_zygote_umount` (id 5) extends the detach to uid 1053, the WebView zygote, whose
isolated children inherit the namespace it was handed;
[`kernel/policy/allowlist.c`](../kernel/policy/allowlist.c) special-cases
`WEBVIEW_ZYGOTE_UID` inside `ksu_uid_should_umount()` and returns the feature flag directly,
because 1053 is a system uid with no App Profile of its own to consult. The fork's
`mount_hide` (id 16) works on the other side of the same probe: rather than detaching
anything, it filters the records that `/proc/<pid>/mountinfo`, `mounts` and `mountstats`
render, keyed on the reading task. Neither supersedes the other, and describing them as
duplicates gets both wrong. Unmounting changes what is mounted, for uid 1053 and the
children it forks. Filtering changes what /proc prints, for any isolated reader -- including
an Android 17 `zygote_next` process that reads its own `mountinfo` while still sitting in
init's global namespace, where there is nothing to unmount in the first place.

The `ksud boot-patch -h` output pasted into
[`installation.md`](docs/guide/installation.md) is a transcript of an older binary.
`BootPatchArgs` in [`boot_patch.rs`](../userspace/ksud/src/boot_patch.rs) has no
`--magiskboot` field and ksud does not shell out to magiskboot at all; the block also
omits `--backup`, `--partition`, `--out-name`, `--cmdline`, `--allow-shell`,
`--enable-adbd`, `--adb-debug-prop`, `--no-install`, `--no-custom-rc` and `--ramdisk`. The
separate sections on patching an image by hand with magiskboot remain valid.

A second patcher has since grown up beside it, and the page has no section for it at all.
`ksud boot-patch-v2`, declared in [`cli.rs`](../userspace/ksud/src/cli.rs) and implemented
in [`lkm_image.rs`](../userspace/ksud/src/lkm_image.rs), takes four options -- `--boot`,
`--module`, `--output`, `--force` -- and always operates on a boot image, never selecting
`init_boot` or `vendor_boot` for you, which is why it is a subcommand rather than a flag on
the old one. The mechanism differs more than the flags do. `boot-patch` builds an LKM image
the way the page describes, by displacing the ramdisk's `init` with
[`ksuinit`](../userspace/ksuinit) so that a userspace process loads `kernelsu.ko` during
boot; `boot-patch-v2` leaves the ramdisk alone entirely. It decompresses the kernel out of
the image, recovers the kernel's own symbol table by decoding the compressed `kallsyms`
blobs embedded in it, cross-checks that recovery against the kernel's BTF blob --
`validate_kallsyms_btf_boundaries()` resolves `__start_BTF` and `__stop_BTF` out of the
recovered table and refuses to continue unless the span they describe matches the blob that
`find_btf_candidates()` in [`lkm_image_btf.rs`](../userspace/ksud/src/lkm_image_btf.rs)
located by scanning the image -- relocates the module's ELF against the addresses it just
recovered, and appends the result as a `KSULKM1` capsule with an assembly bootstrap
([`lkm_image_bootstrap.S`](../userspace/ksud/src/lkm_image_bootstrap.S)).
Reading symbols from the image rather than trusting a KMI string is what lets it patch a
kernel that no prebuilt module was published for. The manager does not reach for it --
`KsuCli.kt` still builds a `boot-patch` command line -- so "Use the command line" is the
section that is a subcommand short. "Use the manager" is short an entry of its own. It
lists three install methods; upstream's #3644 added a fourth. `InstallMethod` in
[`InstallUtils.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/screen/install/InstallUtils.kt)
now carries `DownloadFile` beside `SelectFile`, `DirectInstall` and
`DirectInstallToInactiveSlot`, and `InstallScreen.kt` appends it to the option list
unconditionally, so it is on screen for every user, rooted or not: give it a URL and a
partition name and the manager pulls the boot image out of a remote OTA full ZIP and
patches that, with no local firmware download first. Nothing on the page says the option
exists.

[`how-to-integrate-for-non-gki.md`](docs/guide/how-to-integrate-for-non-gki.md) describes
upstream v0.9.5. It has you paste `extern` declarations for `ksu_handle_vfs_read`,
`ksu_handle_stat`, `ksu_handle_faccessat` and `ksu_handle_devpts` into `fs/read_write.c`,
`fs/stat.c`, `fs/open.c` and `fs/devpts/inode.c`, then call them from the VFS helpers; not
one of those symbols exists here. Interception happens instead through a `sys_enter`
[tracepoint][tracepoints] that rewrites a marked task's syscall number into a borrowed
`ni_syscall` slot, so there are no VFS call sites left to patch and none of the page's diffs
will apply. [`kernel/hook/README.md`](../kernel/hook/README.md) lays that mechanism out.

One of the page's hunks is worse than merely stale, and it is the newest one to become so.
Its `fs/exec.c` patch declares
`extern int ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr, void *argv, void *envp, int *flags)`.
A symbol of exactly that name exists in this tree -- upstream added it while adapting to the
execveat path that newer bionic takes -- but
[`sucompat.c`](../kernel/feature/sucompat.c) defines it as
`long ksu_handle_execveat_sucompat(const char __user **filename_user, int orig_nr, struct pt_regs *regs)`,
a dispatcher entry point taking the original syscall number and a register frame. C performs
no cross-translation-unit check on a hand-written `extern`, so the guide's declaration
compiles, links, and calls that function with five arguments it was never written to accept.
The page also names `kernel/ksu.c`, which is now
[`kernel/core/init.c`](../kernel/core/init.c).

Nothing in the guide covers the feature layer, and the omission is not only about the fork.
Seven of the mechanisms in [`kernel/feature/`](../kernel/feature/) register a feature id and
so are switchable from `ksud feature set`: `su_compat`, `kernel_umount`,
`webview_zygote_umount`, `adb_root`, `selinux_hide` and `sulog` are upstream's, `mount_hide`
is the fork's. Three more carry no id at all, because none of them is a global on/off
switch: `mem_spoof`, `ptctl` and `uhook` each aim at one target at a time and take their
commands over the IOCTL interface instead -- `KSU_IOCTL_SET_SPOOF_MEM` for the first,
`KSU_IOCTL_PTCTL` and `KSU_IOCTL_UHOOK` for the other two, both `_IOWR` carrying a
per-feature command struct so the call can report back in the same buffer. All three are
declared in [`uapi/supercall.h`](../uapi/supercall.h) and dispatched from
[`kernel/supercall/dispatch.c`](../kernel/supercall/dispatch.c), which also owns
`KSU_IOCTL_SET_SPOOF_VERSION` and `KSU_IOCTL_SET_SPOOF_CPU`, the UTS and CPU spoofing
supercalls -- those two have their handlers in that file rather than under
`kernel/feature/`, which is why they have no section in the feature README. All ten do have
sections in [`kernel/feature/README.md`](../kernel/feature/README.md), and `ptctl` and
`uhook` additionally have a caller-facing reference in
[`docs/instrumentation.md`](../docs/instrumentation.md). None of them has a page a user
would find.

## See also

- [`../docs/architecture.md`](../docs/architecture.md) -- the repository-wide hub
- [`../scripts/README.md`](../scripts/README.md) -- build and packaging automation
- [`../manager/README.md`](../manager/README.md) -- the template endpoint's client
- [`../userspace/ksud/README.md`](../userspace/ksud/README.md) -- the CLI the guide covers
- [`../uapi/README.md`](../uapi/README.md) -- the ABI behind the feature ids named above
- [`../kernel/feature/README.md`](../kernel/feature/README.md) -- the feature layer, upstream's and the fork's

<!-- reference links: kernel documentation and man pages -->
[selinux]: https://docs.kernel.org/admin-guide/LSM/SELinux.html
[setresuid-2]: https://man7.org/linux/man-pages/man2/setresuid.2.html
[tracepoints]: https://docs.kernel.org/trace/tracepoints.html
