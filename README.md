## What is this?
This project embeds LibreOfficeKit into Electron as a Blink WebPlugin

## How do I contribute?
Install the [Electron prerequisites](https://www.electronjs.org/docs/latest/development/build-instructions-gn#platform-prerequisites) for your operating system

Sync the code (including the git cache, uses ~42 GiB of space):

``` bash
git clone https://github.com/coparse-inc/electron-libreoffice
cd electron-libreoffice
scripts/e sync
```

## How do I build it?

For local testing: `scripts/e build`

For release builds: `IS_RELEASE=true scripts/e build`

For compiling for Apple Silicon: `FOR_APPLE_SILICON=true scripts/e build`

## How do I pull the upstream changes from Electron?

Run `scripts/pull-upstream-changes.sh`
