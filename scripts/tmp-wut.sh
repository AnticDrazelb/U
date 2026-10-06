cd ~/cemups4-build/src && ([ -d wut ] || git clone -q --depth 1 https://github.com/devkitPro/wut.git) && cd wut
find samples -maxdepth 3 | grep -i -E "gx2|triangle|gfx|shader" ; find . -name "*.gsh" | head
grep -n "GLSL\|GFD\|LoadGFD\|Shader" include/whb/gfx.h | head -30
