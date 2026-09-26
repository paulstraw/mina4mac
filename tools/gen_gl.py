"""Generate the OpenGL bridge: __stdcall host thunks for opengl32.dll that call the host's (legacy 2.1 context)
OpenGL, marshalling arguments from the 32-bit guest stack.

Which names: every GL command named in noita.exe (its GL loader's GetProcAddress list, 1,044 names) that the
macOS legacy headers (OpenGL/gl.h + glext.h) declare. The rest aren't provided: GetProcAddress returns NULL
for them (runtime/opengl32.c gl_proc_address), as a driver without them would. Signatures come from the
Khronos registry, third_party/khronos/gl.xml; calls are type-checked against the SDK prototypes.

Marshalling, by gl.xml parameter type:
  - integer scalars: one stack slot; GLfloat one slot; GLdouble and 64-bit integers two slots;
    GLintptr/GLsizeiptr one slot, sign-extended;
  - GLsync: a 32-bit guest handle (gl_sync_host/gl_sync_guest in runtime/opengl32.c);
  - pointers to scalars: guest pointers (MEM + p; same element sizes on both sides). Except the ones that are
    buffer offsets when a buffer is bound (gl_buf_ptr): gl*Pointer (GL_ARRAY_BUFFER), element indices
    (GL_ELEMENT_ARRAY_BUFFER), and pixel data read from GL_PIXEL_UNPACK_BUFFER or written to GL_PIXEL_PACK_BUFFER;
  - void ** results (glGetPointerv, glGetVertexAttribPointerv): converted back to guest pointers or offsets;
  - no current context: nothing happens and the result is 0, as opengl32 does on Windows (the game's GL
    loader makes GL calls before it creates its window).
Hand-written in runtime/opengl32.c: HAND below. Exits non-zero if a name can't be marshalled.

Writes build/gen_all/gl_gen.c.

  uv run tools/gen_gl.py
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from glsurvey import header_names  # noqa: E402
from pe import ROOT, game_dir  # noqa: E402

GEN = ROOT / "build/gen_all"
XML = ROOT / "third_party/khronos/gl.xml"  # KhronosGroup/OpenGL-Registry @ 1cdd228e (Apache-2.0)
# Hand-written in runtime/opengl32.c: strings, buffer mapping, arrays of pointers, and sync deletion.
HAND = {"glGetString", "glShaderSource", "glMapBuffer", "glUnmapBuffer", "glGetBufferPointerv", "glDeleteSync",
        "glMultiDrawElements", "glMultiDrawElementsBaseVertex"}
INT = {"GLenum", "GLint", "GLuint", "GLsizei", "GLboolean", "GLbitfield", "GLbyte", "GLubyte", "GLshort",
       "GLushort", "GLchar", "GLhalf", "GLclampx", "GLfixed"}
WIDE = {"GLdouble": "ARG_F64", "GLclampd": "ARG_F64", "GLint64": "ARG_I64", "GLuint64": "ARG_I64"}
FLOAT = {"GLfloat", "GLclampf"}
PTRSIZED = {"GLintptr", "GLsizeiptr"}
# The pointer argument (by name) that is a buffer offset while the binding is non-zero.
ARRAY = {f"gl{k}Pointer" for k in ("Vertex", "Color", "TexCoord", "Normal", "SecondaryColor", "FogCoord", "EdgeFlag",
                                   "Index", "VertexAttrib", "VertexAttribI")} | {"glInterleavedArrays"}
UNPACK = {f"gl{c}Tex{s}Image{d}D" for c in ("", "Compressed") for s in ("", "Sub") for d in (1, 2, 3)}
UNPACK |= {"glDrawPixels", "glBitmap", "glPolygonStipple", "glPixelMapfv", "glPixelMapuiv", "glPixelMapusv"}
PACK = {"glReadPixels", "glGetTexImage", "glGetCompressedTexImage", "glGetPolygonStipple", "glGetPixelMapfv",
        "glGetPixelMapuiv", "glGetPixelMapusv"}
POINTER_PARAMS = {"pointer", "indices", "pixels", "data", "img", "bitmap", "mask", "values"}


def binding(name):
    if name in ARRAY:
        return "GL_ARRAY_BUFFER_BINDING"
    if re.fullmatch(r"glDraw(Range)?Elements\w*", name):
        return "GL_ELEMENT_ARRAY_BUFFER_BINDING"
    if name in UNPACK:
        return "GL_PIXEL_UNPACK_BUFFER_BINDING"
    if name in PACK:
        return "GL_PIXEL_PACK_BUFFER_BINDING"
    return None


def commands():
    """name -> (return type, [(type, param name)]) from gl.xml."""
    out = {}
    for c in ET.parse(XML).getroot().find("commands"):
        proto = c.find("proto")
        name = proto.find("name").text
        ret = " ".join("".join(proto.itertext()).rsplit(name, 1)[0].split())
        params = []
        for p in c.findall("param"):
            pn = p.find("name").text
            params.append((" ".join("".join(p.itertext()).rsplit(pn, 1)[0].replace("*", " *").split()), pn))
        out[name] = (ret, params)
    return out


def game_names():
    """GL command names in noita.exe (its GL loader's name table)."""
    exe = (game_dir() / "noita.exe").read_bytes()
    return {m.decode() for m in re.findall(rb"(?<=\0)(gl[A-Z]\w+)(?=\0)", exe)}


def thunk(name, ret, params):
    args, pre, post, slot = [], [], [], 0
    bind = binding(name)
    for t, pn in params:
        a = f"ARG({slot})"
        slot += 1
        if t in INT:
            args.append(f"({t}){a}")
        elif t in FLOAT:
            args.append(f"ARG_F32({slot - 1})")
        elif t in WIDE:
            args.append(f"({t}){WIDE[t]}({slot - 1})")
            slot += 1
        elif t in PTRSIZED:
            args.append(f"({t})(int32_t){a}")
        elif t == "GLsync":
            args.append(f"gl_sync_host({a})")
        elif t == "void * *":
            pre.append(f"void *p{slot - 1} = NULL;")
            args.append(f"&p{slot - 1}")
            post.append(f"if ({a}) wr32({a}, gl_guest_ptr(p{slot - 1}));")
        elif t.count("*") == 1 and t.endswith("*") and t.replace("const", "").replace("*", "").strip() in INT | FLOAT | set(WIDE) | {"void"}:
            if bind and pn in POINTER_PARAMS:
                args.append(f"gl_buf_ptr({bind}, {a})")
            else:
                args.append(f"ARG_PTR({slot - 1})")
        else:
            raise ValueError(f"parameter {pn}: {t}")
    call = f"{name}({', '.join(args)})"
    if ret == "void":
        body = [*pre, f"{call};", *post]
    elif ret in INT:
        body = [*pre, f"ret_i32(c, (uint32_t){call});", *post]
    elif ret == "GLsync":
        body = [f"ret_i32(c, gl_sync_guest({call}));"]
    else:
        raise ValueError(f"returns {ret}")
    zero = "" if ret == "void" else " ret_i32(c, 0);"
    return f"HOST_STDCALL(opengl32, {name}, {4 * slot}) {{{zero} if (GL_CTX) {{ {' '.join(body)} }} }}"


def generate():
    """Write gl_gen.c; returns (game names, bridged names, generated count)."""
    cmds, names = commands(), game_names()
    unknown = sorted(n for n in names if n not in cmds)
    if unknown:
        sys.exit(f"not in gl.xml: {unknown}")
    bridged = sorted(names & header_names("gl.h", "glext.h"))
    out, errors = [], []
    for n in bridged:
        if n in HAND:
            continue
        try:
            out.append(thunk(n, *cmds[n]))
        except ValueError as e:
            errors.append(f"{n}: {e}")
    if errors:
        sys.exit("can't marshal:\n  " + "\n  ".join(errors))
    GEN.mkdir(parents=True, exist_ok=True)
    (GEN / "gl_gen.c").write_text("\n".join([
        "// Generated by tools/gen_gl.py from gl.xml: __stdcall host thunks for opengl32.dll.",
        '#include "opengl32.h"', "", *out, ""]))
    return names, bridged, len(out)


def main():
    names, bridged, n = generate()
    print(f"opengl32: {len(names)} GL names in noita.exe, {len(bridged)} in the legacy headers: {n} generated, "
          f"{len(HAND)} hand-written; wrote {GEN / 'gl_gen.c'}")


if __name__ == "__main__":
    main()
