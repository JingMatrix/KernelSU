# Library for KernelSU's module WebUI

`kernelsu` is the npm package a module's WebUI imports to reach the manager. Every export
is a thin wrapper over `window.ksu`, the `@JavascriptInterface` object the manager injects
into the WebView: [`index.js`](index.js) serialises the arguments to JSON, hands the bridge
the name of a callback, and turns the script the host injects back into a promise or an
event. [`index.d.ts`](index.d.ts) is the whole type surface, and there is nothing else in
the package.

The other half of every function below is
[`WebViewInterface.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewInterface.kt)
in the manager app. How a page is loaded, served and given a root shell in the first place
is described in [the manager README](../manager/README.md#the-webui-host): the page comes
from the module's `/data/adb/modules/<id>/webroot`, is served over the virtual origin
`https://mui.kernelsu.org/`, and runs commands as root with no allowlist and no per-module
capability model. Read that section before this one.

- [Install](#install) -- and what this fork does not publish
- [API](#api)
  - [`exec`](#exec), [`spawn`](#spawn), [`ChildProcess`](#childprocess) -- running commands
  - [`fullScreen`](#fullscreen), [`enableEdgeToEdge`](#enableedgetoedge) -- window chrome,
    insets and colours
  - [`toast`](#toast), [`exit`](#exit) -- host UI
  - [`moduleInfo`](#moduleinfo) -- what this module is
  - [`listPackages`](#listpackages), [`getPackagesInfo`](#getpackagesinfo) -- the app list
- [See also](#see-also)

## Install

```sh
yarn add kernelsu
```

This directory is the source of that package -- [`package.json`](package.json) declares
`kernelsu` 3.0.2 -- but its `repository`, `bugs` and `homepage` all point at upstream
`tiann/KernelSU`, and no workflow in [`../.github/workflows/`](../.github/workflows) runs
`npm publish`. Nothing in this fork ships to the registry, so what `yarn` installs need not
match the API below. Depend on `js/` directly when you need this tree's copy.

## API

The wrappers call methods on `window.ksu`, which exists only inside the manager's WebView.
Feature-detect anything you are not sure the installed manager implements, with
`typeof ksu.<name> === 'function'`, before relying on it.

### exec

Runs `command` in a root shell and returns a Promise that resolves when the shell has
finished with it.

- `command` `<string>` The command to run, with space-separated arguments.
- `options` `<Object>`
  - `cwd` - Prepended to the command as `cd <cwd>;`. The value is not quoted and the result
    is not checked, so a directory that does not exist leaves the command running in the
    shell's default directory rather than failing.
  - `env` - Prepended as one `export <key>=<value>;` per entry, also unquoted, so a value
    containing a space or a shell metacharacter is interpreted by the shell.

The promise resolves with three fields:

| Field | Type | What it holds |
| --- | --- | --- |
| `errno` | `number` | The shell's exit status for the whole command line. The name is a misnomer: this is libsu's `Shell.Result.code`, not an `errno` value. |
| `stdout` | `string` | The lines the command wrote to stdout, joined with `\n`. Terminators are not preserved, so a trailing newline is lost. |
| `stderr` | `string` | The same, for stderr. |

```javascript
import { exec } from 'kernelsu';

const { errno, stdout, stderr } = await exec('ls -l', { cwd: '/tmp' });
if (errno === 0) {
    // success
    console.log(stdout);
}
```

The promise rejects only when the bridge call itself throws, for instance because
`window.ksu` is absent. A command that fails resolves, with a non-zero `errno`.

#### The shell a call gets

`exec` and [`spawn`](#spawn) each open a shell for that one call and close it again when
the command finishes, so no working directory, environment variable or background job
survives into the next call.
[`createRootShell`](../manager/app/src/main/java/me/weishu/kernelsu/ui/util/KsuCli.kt)
builds it in three attempts, taking the first that starts:

| Attempt | Command | What you get |
| --- | --- | --- |
| 1 | `<nativeLibraryDir>/libksud.so debug su -g` | Root through KernelSU |
| 2 | `su -mm` | Root through whatever other `su` the device has |
| 3 | `sh` | Not root -- and the command still runs |

The first attempt reaches [`grant_root`](../userspace/ksud/src/su.rs), where `-g` means the
shell moves into pid 1's [mount namespace][mount-namespaces-7] before `sh` is exec'd, and
`/data/adb/ksu/bin` is appended to its [`PATH`][environ-7]. Commands therefore see the
global mount view rather than the WebView's own. The fall through to attempt 3 is silent,
so treat `errno` as the only evidence that a command ran, and do not assume it ran as root.

### spawn

Runs `command` with `args` in a root shell of its own and returns a
[`ChildProcess`](#childprocess) that emits the output as it arrives. If omitted, `args`
defaults to an empty array.

- `command` `<string>` The command to run.
- `args` `<string[]>` List of string arguments. The host joins `command` and `args` with
  single spaces into one command line and quotes nothing, so an argument containing a
  space, a quote or another [shell][sh-1p] metacharacter is re-split and expanded before
  the command sees it. Quote it yourself when it might.
- `options` `<Object>`:
  - `cwd` `<string>` - Prepended as `cd <cwd>;`, exactly as in [`exec`](#exec).
  - `env` `<Object>` - Prepended as one `export <key>=<value>;` per entry, likewise.

The name is Node's; the semantics are not. There is no separate process to address: the
command line goes to the root shell as a single job, and everything the page sees arrives
as script the host injects into the page.

Example of running `ls -lh /data`, capturing `stdout`, `stderr`, and the exit code:

```javascript
import { spawn } from 'kernelsu';

const ls = spawn('ls', ['-lh', '/data']);

ls.stdout.on('data', (data) => {
  console.log(`stdout: ${data}`);
});

ls.stderr.on('data', (data) => {
  console.log(`stderr: ${data}`);
});

ls.on('exit', (code) => {
  console.log(`child process exited with code ${code}`);
});
```

`index.d.ts` also declares a `spawn(command, options)` form. `index.js` implements it by
copying `args` into `options` and leaving `args` pointing at the same object, which is then
stringified into the parameter the host parses as a JSON array of arguments. Pass an
explicit array -- `spawn('ls', [], opts)` -- rather than relying on that form.

#### ChildProcess

The object [`spawn`](#spawn) returns: a plain emitter defined in [`index.js`](index.js),
not a Node `ChildProcess`.

| Member | What it is |
| --- | --- |
| `stdout` | [Emitter](#stdout) for standard output |
| `stderr` | [Emitter](#stderr) for standard error |
| `on('exit', cb)` | [The exit status](#event-exit), emitted once |
| `on('error', cb)` | [A failure](#event-error), with the two delivery limits noted there |
| `stdin` | Built by the constructor and inert: nothing writes to the command's stdin |
| `emit(event, ...)` | How the host delivers events; a page has no reason to call it |

There is no `pid` and no `kill()`. Once a command is running, nothing in this API can
signal or stop it.

##### Event 'exit'

- `code` `<number>` The exit status the root shell reported for the command line.

Emitted when the command finishes. `code` is always a number: the host interpolates
`Shell.Result.code` straight into the injected script, and nothing in the bridge or in
[`index.js`](index.js) emits `'exit'` with `null`. This is the completion signal and,
because [`'error'`](#event-error) is usually undeliverable, the failure signal too --
test `code`.

The wrapper's own `'exit'` listener deletes the callback object the host addresses, so no
further `'data'` or `'error'` event can be delivered after this one.

##### Event 'error'

- `err` `<Error>` An `Error` carrying `exitCode`, the non-zero status, and `message`, the
  stderr collected for the whole command.

The host emits this when the command exits with a non-zero status. Two limits make it
unreliable, and both are structural rather than incidental:

- The host posts the `'error'` script after the `'exit'` script. By the time it runs, the
  wrapper's `'exit'` listener has deleted the callback object the script names, so the emit
  throws inside the host's own `try` and is logged to the console as `emitErr`.
- The failure-to-spawn case is emitted synchronously inside `spawn()`, before the object a
  caller could attach a listener to has been returned.

Listen on [`'exit'`](#event-exit) and test `code`.

##### `stdout`

A minimal emitter for the command's standard output. It is not a Node `Readable`: the only
member is `on('data', cb)`, and the callback receives one string for each element the shell
job appends to the collected output. There is no `'end'` or `'close'` event, no `pipe()`,
no `setEncoding()` and no back pressure -- the host pushes each chunk into the page as soon
as it has it.

```javascript
const subprocess = spawn('ls');

subprocess.stdout.on('data', (data) => {
  console.log(`Received chunk ${data}`);
});
```

##### `stderr`

The same minimal emitter, built from the same constructor, carrying the command's standard
error: `on('data', cb)` and nothing else.

### fullScreen

Hide or show the system bars.

```javascript
import { fullScreen } from 'kernelsu';
fullScreen(true);
```

The call does two things. It hides or shows the system bars on the host activity's window,
and only when the WebView's context is an `Activity`; then, outside that check, it passes
the same flag to [`enableEdgeToEdge`](#enableedgetoedge). `fullScreen(false)` therefore
also turns edge-to-edge off, including edge-to-edge the page had enabled by loading
`internal/insets.css`. Re-enable it explicitly if the page still needs it.

### enableEdgeToEdge

Drop the safe-area padding the host puts around the WebView and take the system-bar
geometry as CSS variables instead.

```javascript
import { enableEdgeToEdge } from 'kernelsu';
enableEdgeToEdge(true);
```

- tips: this is disabled by default but if you request resource from `internal/insets.css`,
  this will be enabled automatically.
- To get insets value and enable this automatically, you can
  - add `@import "https://mui.kernelsu.org/internal/insets.css";` in css OR
  - add `<link rel="stylesheet" type="text/css" href="/internal/insets.css" />` in html.

#### One flag, two layouts

`fullScreen`, `enableEdgeToEdge` and the `internal/insets.css` request all write the same
host boolean, which
[`WebUIScreen`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebUIScreen.kt)
reads:

| State | Padding around the WebView | Insets pushed into the page |
| --- | --- | --- |
| Disabled (the default) | `WindowInsets.safeDrawing`: status bar, navigation bar, cutout and keyboard | None |
| Enabled | `WindowInsets.ime` alone, which is zero whenever the keyboard is hidden | `--safe-area-inset-*`, re-applied on every navigation |

An edge-to-edge page therefore owns its entire safe area and must lay itself out from those
variables. The stylesheet from `internal/insets.css` defines
`--safe-area-inset-top`, `--safe-area-inset-right`, `--safe-area-inset-bottom` and
`--safe-area-inset-left` in `px`, plus `--window-inset-*` and `--f7-safe-area-*` aliases
defined over them
([`Insets.kt`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/Insets.kt)). When
the geometry changes the host only sets the four `--safe-area-inset-*` properties on
`document.documentElement`; the aliases follow because they are `var()` references.

#### The other synthetic stylesheet

[`SuFilePathHandler`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/SuFilePathHandler.java)
answers a second path before it touches disk: `internal/colors.css` returns the manager's
palette as one `:root` block of Material colour roles -- `--primary`, `--onPrimary`,
`--surface`, `--background` and the rest -- rendered by
[`MonetColorsProvider`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/MonetColorsProvider.kt).
It returns an *empty* stylesheet unless the manager's `color_mode` preference is 3 to 6 or
its skin is Material, so a page that imports it still needs its own fallback colours.
Unlike `insets.css`, requesting it changes no host state.

### toast

Show a toast message.

```javascript
import { toast } from 'kernelsu';
toast('Hello, world!');
```

The host posts it over the WebView with `Toast.LENGTH_SHORT`. Neither the duration nor the
placement can be chosen, and there is no way to dismiss one early.

### moduleInfo

Get module info: a JSON **string** describing the module this page belongs to, not an
object.

```javascript
import { moduleInfo } from 'kernelsu';
// moduleInfo() returns a JSON string, not an object
const info = JSON.parse(moduleInfo());
console.log(info.id, info.moduleDir);
```

The host builds the document from `moduleDir` plus the entry for this module in the output
of `ksud module list`, which
[`list_module`](../userspace/ksud/src/module.rs) assembles:

| Key | Where it comes from |
| --- | --- |
| `moduleDir` | `/data/adb/modules/<id>`, added by the host |
| every key of `module.prop` | The file is parsed as properties and copied through whole; `id` falls back to the directory name when missing or empty |
| `enabled`, `update`, `remove` | Absence of a `disable` file, presence of an `update` file, presence of a `remove` file |
| `web`, `action`, `mount` | A `webroot` directory, an `action.sh`, and a `system` directory without `skip_mount` |
| `actionIcon`, `webuiIcon` | Rewritten to an absolute path only when `module.prop` names an icon that exists inside the module directory; absolute paths and `..` are rejected |
| `managedFeatures` | Comma-separated, present only when the module's config sets `manage.<feature>=true` |

Every value is a string, `"true"` and `"false"` included. If the module directory's name
matches no listed module, the document contains `moduleDir` and nothing else.

### listPackages

List installed packages.

- `type` `<string>` The type of packages to list: "user", "system", or "all". The
  comparison is case-insensitive; `"system"` keeps packages flagged `FLAG_SYSTEM`, `"user"`
  keeps those without it, and any other value -- `"all"`, or a typo -- keeps everything.

```javascript
import { listPackages } from 'kernelsu';
// list user packages
const packages = listPackages("user");
```

Returns a sorted array of package names read from the app list the manager caches in
[`SuperUserViewModel`](../manager/app/src/main/java/me/weishu/kernelsu/ui/viewmodel/SuperUserViewModel.kt),
not queried from the platform on the spot. Three consequences follow from that cache:

- Only packages that carry code appear;
  [`SuperUserRepositoryImpl`](../manager/app/src/main/java/me/weishu/kernelsu/data/repository/SuperUserRepositoryImpl.kt)
  filters on `FLAG_HAS_CODE`.
- The manager's own list holds one synthetic row that is not a package -- the WebView
  zygote, uid 1053, carried so its App Profile is reachable -- and both this call and
  [`getPackagesInfo`](#getpackagesinfo) drop rows flagged `special`. A page never sees it.
- The cache is gathered for every alive Android user and concatenated
  ([`KsuService`](../manager/app/src/main/java/me/weishu/kernelsu/ui/KsuService.kt)), and
  the result is sorted but never deduplicated, so a package installed for several users
  appears once per user.
- An empty array does not distinguish "nothing matched" from "the call failed": the wrapper
  returns `[]` when the bridge throws, and the host returns an empty list when the cache
  never loaded. The WebUI host does fill the cache before it creates the WebView if it is
  empty, so this is the failure case, not the cold-start case.

#### App icons

- tips: when `listPackages` api is available, you can use ksu://icon/{packageName} to get
  app icon. Test that with `typeof ksu.listPackages === 'function'`.

``` javascript
img.src = "ksu://icon/" + packageName;
```

The icon comes from the same cached list, intercepted in
[`WebViewHelper`](../manager/app/src/main/java/me/weishu/kernelsu/ui/webui/WebViewHelper.kt):
a known package yields a 512 px PNG served with `Access-Control-Allow-Origin: *`, an
unknown one a 404 with the `text/plain` body `No such package`, which an `<img>` shows
as a broken image. Supply your own placeholder if that matters.

### getPackagesInfo

Get information for a list of packages.

- `packages` `<string[]>` The list of package names.

```javascript
import { getPackagesInfo } from 'kernelsu';
const packages = getPackagesInfo(['com.android.settings', 'com.android.shell']);
```

Returns an array with one entry per requested name, in the order passed. An entry is a
[`PackagesInfo`](#packagesinfo) for a package the manager has cached, and
`{ packageName, error: "Package not found or inaccessible" }` for anything else, so test
for `error` before reading any other field. If the bridge call throws, the wrapper returns
`[]` for the whole call. The lookup is keyed by package name, so where
[`listPackages`](#listpackages) reports a package once per Android user, only one of those
entries is reachable here; the synthetic non-package row is filtered out before the map is
built, so its empty package name cannot be looked up either.

#### PackagesInfo

An object contains:

| Field | Type | Notes |
| --- | --- | --- |
| `packageName` | `<string>` | Package name of the application. |
| `versionName` | `<string>` | Version of the application; `""` when the platform reports none. |
| `versionCode` | `<number>` | Version code of the application, in its long form. |
| `appLabel` | `<string>` | Display name of the application, as the manager resolved it. |
| `isSystem` | `<boolean>` | Whether the application is a system app; `null` when the platform returned no `ApplicationInfo`. |
| `uid` | `<number>` | UID of the application; `null` in that same case. |

### exit

Exit the current WebUI activity.

```javascript
import { exit } from 'kernelsu';
exit();
```

The bridge raises a close event on the host state rather than ending the process; the
activity finishes on the next composition and disposes the WebView and the root shell that
was serving its files.

## See also

- [`../manager/README.md`](../manager/README.md#the-webui-host) -- the host: activity,
  asset loader, and the shell every call opens
- [`../userspace/ksud/README.md`](../userspace/ksud/README.md) -- `module list`, whose
  output [`moduleInfo`](#moduleinfo) returns, and the `su` this page's shells run
- [`../website/docs/guide/module-webui.md`](../website/docs/guide/module-webui.md) -- the
  module author's view of the same API
- [`../docs/architecture.md`](../docs/architecture.md) -- how the manager, ksud and the
  kernel module fit together

<!-- reference links: man pages -->
[environ-7]: https://man7.org/linux/man-pages/man7/environ.7.html
[mount-namespaces-7]: https://man7.org/linux/man-pages/man7/mount_namespaces.7.html
[sh-1p]: https://man7.org/linux/man-pages/man1/sh.1p.html
