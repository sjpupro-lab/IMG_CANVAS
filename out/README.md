# out/

Runtime output space. **Everything under this directory except this README is
gitignored.** Intentional — models, frames, and demo renders vary per user /
per run and should never be committed.

Layout the wrappers in `scripts/windows/` use:

```
out/
├── models/                 train.ps1 drops <Name>.spai / <Name>.imem here
│   ├── characters.spai
│   └── characters.imem
├── draw/                   draw.ps1 drops one subdir per run
│   └── <Name>/
│       ├── frames/
│       │   ├── frame_000.{png,ppm}
│       │   ├── frame_001.{png,ppm}
│       │   └── ...
│       └── final.{png,ppm}  <- the result (same as the last frame)
└── demo/                   demo.ps1 drops <Name>_{plain,masked}.{png,ppm}
    ├── hero_plain.png
    └── hero_masked.png
```

If you want to share an output, copy it to `assets/` (not gitignored) and
commit it there.
