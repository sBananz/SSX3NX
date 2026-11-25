# SSX3NX build image: the stock devkitPro Switch toolchain plus the host tools
# SSX3 needs that it lacks (PORTING_PLAN.md F6):
#   glslang-tools  glslangValidator, GLSL -> SPIR-V for src/vk (SPIR-V is
#                  target-independent, so it is compiled on the build host)
#   python3-*      lets scripts/regen.sh run inside the container too
#
#   docker build -t ssx3nx-build .
FROM devkitpro/devkita64

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        glslang-tools python3 python3-capstone python3-numpy python3-pil \
 && rm -rf /var/lib/apt/lists/*
