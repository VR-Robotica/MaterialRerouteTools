# Making the Fab upload zips

Maintainer notes; this folder is not part of the Fab package.

`fab_zip.py` builds the Fab upload zip for one engine version from the last commit (commit first: uncommitted
changes are left out). Run it from the plugin folder with Python 3:

```
python Scripts/fab_zip.py
python Scripts/fab_zip.py 5.7
python Scripts/fab_zip.py 5.6 5.8
python Scripts/fab_zip.py 5.6 D:/Builds/MaterialRerouteTools_5.6.zip
```

With no version it makes a zip for every version in `VERSIONS` at the top of the script (5.6, 5.7, 5.8); add a
version there once the plugin builds and loads on it. Each zip is written to the desktop as
`MaterialRerouteTools_<version>.zip` unless a path is given (one version only), and holds one
`MaterialRerouteTools/` folder with the uplugin, README, Config, Resources and Source (no Binaries, Intermediate or
`Scripts/`), the uplugin's `EngineVersion` set to `<version>.0`. The script stops with an error if a path uses
backslashes, build output got in, or a source file is missing its copyright line. Submit one zip per engine version.
