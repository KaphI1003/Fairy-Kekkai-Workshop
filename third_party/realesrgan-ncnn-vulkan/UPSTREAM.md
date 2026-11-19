Source: https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan
Commit: 37026f49824c5cf84062e7c6a5dd71445dcf610f (2022-04-24 release core).
License: MIT, preserved in LICENSE.

Local adaptations: compile unchanged GLSL sources with ncnn's runtime compiler
instead of a separate glslangValidator; cap CPU operator threads to one; check
model/shader loading errors and permit cleanup of a partially loaded model.
Inference architecture, tile/prepadding policy and weights remain upstream.
The shader-header generator is from the MIT-licensed RIFE-ncnn-vulkan source
already vendored in this repository. Windows dependencies are pinned and hashed
by scripts/build-superres-worker.ps1 (ncnn SDK 20221128, Vulkan headers 1.3.231).

stb_image_write.h is unmodified from the same upstream commit, SHA256
51998500E9519A85BE1AA3291C6AD57DEB454DA98A1693AB5230F91784577479.
Its MIT/public-domain alternatives are preserved in STB-LICENSE.txt. Both image
workers use it only for the PNG compatibility route; normal pixels use RGB24
shared-memory mappings and the independent read/infer/store worker stages in
cpp/video_frames.
