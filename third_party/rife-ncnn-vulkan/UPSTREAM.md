Unmodified RIFE inference and shader sources from nihui/rife-ncnn-vulkan,
commit a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7 (MIT, see LICENSE).
https://github.com/nihui/rife-ncnn-vulkan/tree/a7532fc3f9f8f008cd6eecd6f2ffe2a9698e0cf7

The Workshop bridge is maintained in cpp/engine. It keeps one model loaded for
the task and accepts explicit frame-pair/timestamp jobs, preserving exact target
frame times across batches and bypassing held frames and detected scene cuts.
