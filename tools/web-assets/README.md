# web-assets generator

`gen_web_assets.py` turns the frontend's `dist/` into the C table the firmware
serves and a JSON manifest the mock serves the same files from. The policy it
implements, and why, is in
[`modules/web-assets/README.md`](../../modules/web-assets/README.md).

```sh
python gen_web_assets.py --dist ../../src/web/frontend/dist \
    --out-c web_assets_data.c --out-manifest manifest.json --budget-kib 512
python -m pytest tests
```

The application's CMakeLists runs it at build time. `placeholder/` is the page a
firmware built with `APP_WEB_UI_PLACEHOLDER` embeds instead of the frontend.
