# devkitARM + CTRPluginFramework + 3gxtool + makerom, for building .3dsx, .3gx and .cxi (sysmodule).
# Build: docker build -t 3dsrec-dev tools/
FROM devkitpro/devkitarm:latest

# libctrpf comes from the CTRPF author's pacman repo ("any" arch package).
RUN printf '\n[thepixellizeross-lib]\nSigLevel = Optional TrustAll\nServer = https://thepixellizeross.gitlab.io/packages/any\n' \
        >> /opt/devkitpro/pacman/etc/pacman.conf \
 && dkp-pacman -Sy --noconfirm libctrpf \
 && dkp-pacman -Scc --noconfirm

# 3gxtool has no arm64 package: build it from a pinned commit.
ARG GXTOOL_COMMIT=57e3160ab72d8533a2f0601efdc8a149b4825ea5
RUN git clone https://gitlab.com/thepixellizeross/3gxtool.git /tmp/3gxtool \
 && cd /tmp/3gxtool \
 && git checkout "$GXTOOL_COMMIT" \
 && git submodule update --init --recursive --depth 1 \
 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && install -m 755 "$(find build -maxdepth 2 -type f -name 3gxtool | head -1)" /opt/devkitpro/tools/bin/3gxtool \
 && rm -rf /tmp/3gxtool

# makerom (Project_CTR) builds the sysmodule .cxi: pinned release tag.
ARG MAKEROM_TAG=makerom-v0.18.4
RUN git clone --depth 1 --branch "$MAKEROM_TAG" https://github.com/3DSGuy/Project_CTR.git /tmp/pctr \
 && make -C /tmp/pctr/makerom deps -j"$(nproc)" \
 && make -C /tmp/pctr/makerom program -j"$(nproc)" \
 && install -m 755 "$(find /tmp/pctr/makerom -type f -name makerom -perm -u+x | head -1)" /opt/devkitpro/tools/bin/makerom \
 && rm -rf /tmp/pctr

ENV PATH="/opt/devkitpro/tools/bin:${PATH}"
