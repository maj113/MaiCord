#!/usr/bin/env python3
"""Checks the declarations of the live video decoders' libraries against the installed headers: the analogue of
tools/check_pulse_abi.py for include/maicord/platform/decode/vaapi/abi.hpp (libva-dev) and
include/maicord/platform/decode/software/abi.hpp (libdav1d-dev, libopenh264-dev).

None of the three libraries is linked or included: MaiCord dlopen()s them and declares what it uses. Nothing at
build time proves those declarations match, so this script does:

  * every constant whose comment names a library macro or enumerator (`// VA_PICTURE_H264_INVALID`) is compiled
    against the real headers in a _Static_assert, so a value that differs fails;
  * the sizes and field offsets of every struct the headers share with the libraries are computed from the real
    headers (a C program) and asserted on the in-tree declarations (a C++ translation unit): the VA buffers and the
    DRM descriptor equal, the structs of which only the leading fields are declared (dav1d, openh264) at the same
    offsets with the real size at most the declared storage;
  * the order and width of the bit fields of every VA parameter structure equal the library's;
  * every function of the MAICORD_VA_FUNCTIONS, MAICORD_VA_DRM_FUNCTIONS and MAICORD_DAV1D_FUNCTIONS tables is found
    in the preprocessed headers with the same argument count and the same kind (pointer, integer of a size, void) of
    its return type and arguments, and the OpenH264 vtable and the two exported functions the same.

    check_video_abi.py [--include DIR]... [--compiler CC] [--cxx CXX]

With no --include the system headers (/usr/include) are used; CI installs libva-dev, libdav1d-dev and libopenh264-dev
and runs this beside the build.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VA_HEADER = os.path.join(ROOT, "include", "maicord", "platform", "decode", "vaapi", "abi.hpp")
SOFTWARE_HEADER = os.path.join(ROOT, "include", "maicord", "platform", "decode", "software", "abi.hpp")

VA_INCLUDES = ("va/va.h", "va/va_drm.h", "va/va_drmcommon.h", "va/va_dec_hevc.h", "va/va_dec_av1.h")
DAV1D_INCLUDES = ("errno.h", "dav1d/dav1d.h", "dav1d/data.h", "dav1d/picture.h")
OPENH264_INCLUDES = ("wels/codec_api.h",)

# Integer sizes of the typedefs and spellings either side uses.
SCALARS = {
    "int": 4, "unsigned": 4, "unsigned int": 4, "long": 8, "unsigned long": 8, "long long": 8, "size_t": 8,
    "usize": 8, "u8": 1, "u16": 2, "u32": 4, "u64": 8, "i8": 1, "i16": 2, "i32": 4, "i64": 8,
    "uint8_t": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8, "int32_t": 4, "ptrdiff_t": 8,
    "VAStatus": 4, "VAProfile": 4, "VAEntrypoint": 4, "VAConfigAttribType": 4, "VABufferType": 4,
    "VAConfigID": 4, "VAContextID": 4, "VABufferID": 4, "VASurfaceID": 4, "VAGenericID": 4,
    "Status": 4, "ConfigId": 4, "ContextId": 4, "BufferId": 4, "SurfaceId": 4,
    "DECODING_STATE": 4, "DECODER_OPTION": 4,
    "bool": 1,
}
POINTER_TYPEDEFS = {"VADisplay", "Display", "ISVCDecoder", "IsvcDecoder"}

VA_STRUCTS = {
    "ConfigAttrib": "VAConfigAttrib", "GenericValue": "VAGenericValue", "SurfaceAttrib": "VASurfaceAttrib",
    "DrmPrimeDescriptor": "VADRMPRIMESurfaceDescriptor", "PictureH264": "VAPictureH264",
    "PictureParameterH264": "VAPictureParameterBufferH264", "IqMatrixH264": "VAIQMatrixBufferH264",
    "SliceParameterH264": "VASliceParameterBufferH264", "PictureHevc": "VAPictureHEVC",
    "PictureParameterHevc": "VAPictureParameterBufferHEVC", "SliceParameterHevc": "VASliceParameterBufferHEVC",
    "IqMatrixHevc": "VAIQMatrixBufferHEVC", "SegmentationAv1": "VASegmentationStructAV1",
    "FilmGrainAv1": "VAFilmGrainStructAV1", "WarpedMotionAv1": "VAWarpedMotionParamsAV1",
    "PictureParameterAv1": "VADecPictureParameterBufferAV1", "SliceParameterAv1": "VASliceParameterBufferAV1",
}
# In-tree field name -> expression of the library's struct (where the spelling differs).
VA_FIELDS = {
    ("PictureH264", "top_field_order_cnt"): "TopFieldOrderCnt", ("PictureH264", "bottom_field_order_cnt"): "BottomFieldOrderCnt",
    ("PictureParameterH264", "curr_pic"): "CurrPic", ("PictureParameterH264", "reference_frames"): "ReferenceFrames",
    ("SliceParameterH264", "ref_pic_list0"): "RefPicList0", ("SliceParameterH264", "ref_pic_list1"): "RefPicList1",
    ("PictureParameterHevc", "curr_pic"): "CurrPic", ("PictureParameterHevc", "reference_frames"): "ReferenceFrames",
    ("SliceParameterHevc", "ref_pic_list"): "RefPicList", ("SliceParameterHevc", "long_slice_flags"): "LongSliceFlags",
    ("SliceParameterHevc", "chroma_offset_l0"): "ChromaOffsetL0", ("SliceParameterHevc", "chroma_offset_l1"): "ChromaOffsetL1",
    ("IqMatrixH264", "scaling_list_4x4"): "ScalingList4x4", ("IqMatrixH264", "scaling_list_8x8"): "ScalingList8x8",
    ("IqMatrixHevc", "scaling_list_4x4"): "ScalingList4x4", ("IqMatrixHevc", "scaling_list_8x8"): "ScalingList8x8",
    ("IqMatrixHevc", "scaling_list_16x16"): "ScalingList16x16", ("IqMatrixHevc", "scaling_list_32x32"): "ScalingList32x32",
    ("IqMatrixHevc", "scaling_list_dc_16x16"): "ScalingListDC16x16", ("IqMatrixHevc", "scaling_list_dc_32x32"): "ScalingListDC32x32",
}
VA_OFFSETS = [
    ("DrmPrimeDescriptor", f) for f in ("num_objects", "objects", "num_layers", "layers")
] + [
    ("PictureH264", f) for f in ("frame_idx", "flags", "top_field_order_cnt", "bottom_field_order_cnt")
] + [
    ("PictureParameterH264", f) for f in ("curr_pic", "reference_frames", "picture_width_in_mbs_minus1", "num_ref_frames",
                                          "seq_fields", "pic_init_qp_minus26", "pic_fields", "frame_num")
] + [
    ("SliceParameterH264", f) for f in ("slice_data_bit_offset", "slice_type", "ref_pic_list0", "ref_pic_list1",
                                        "luma_log2_weight_denom", "luma_weight_l0", "chroma_weight_l0", "luma_weight_l1",
                                        "chroma_offset_l1")
] + [
    ("PictureParameterHevc", f) for f in ("curr_pic", "reference_frames", "pic_width_in_luma_samples", "pic_fields",
                                          "sps_max_dec_pic_buffering_minus1", "column_width_minus1", "row_height_minus1",
                                          "slice_parsing_fields", "log2_max_pic_order_cnt_lsb_minus4", "st_rps_bits")
] + [
    ("SliceParameterHevc", f) for f in ("slice_data_byte_offset", "slice_segment_address", "ref_pic_list", "long_slice_flags",
                                        "collocated_ref_idx", "luma_log2_weight_denom", "delta_luma_weight_l0",
                                        "chroma_offset_l0", "chroma_offset_l1", "five_minus_max_num_merge_cand",
                                        "num_entry_point_offsets", "slice_data_num_emu_prevn_bytes")
] + [
    ("PictureParameterAv1", f) for f in ("seq_info_fields", "current_frame", "anchor_frames_list", "frame_width_minus1",
                                         "ref_frame_map", "ref_frame_idx", "seg_info", "film_grain_info", "tile_cols",
                                         "width_in_sbs_minus_1", "height_in_sbs_minus_1", "context_update_tile_id",
                                         "pic_info_fields", "superres_scale_denominator", "filter_level",
                                         "loop_filter_info_fields", "ref_deltas", "base_qindex", "qmatrix_fields",
                                         "mode_control_fields", "cdef_damping_minus_3", "cdef_y_strengths",
                                         "loop_restoration_fields", "wm")
] + [
    ("SliceParameterAv1", f) for f in ("slice_data_offset", "tile_row", "tile_column", "tg_start", "anchor_frame_idx",
                                       "tile_idx_in_tile_list")
] + [
    ("SegmentationAv1", "feature_data"), ("SegmentationAv1", "feature_mask"), ("FilmGrainAv1", "grain_seed"),
    ("FilmGrainAv1", "ar_coeffs_cr"), ("FilmGrainAv1", "cb_offset"), ("FilmGrainAv1", "cr_offset"),
    ("WarpedMotionAv1", "wmmat"), ("WarpedMotionAv1", "invalid"), ("SurfaceAttrib", "value"),
    ("GenericValue", "value"),
]

DAV1D_STRUCTS = {"Dav1dSettings": "Dav1dSettings", "Dav1dData": "Dav1dData", "Dav1dSequenceHeader": "Dav1dSequenceHeader",
                 "Dav1dPicture": "Dav1dPicture"}
# (struct, in-tree field, the library's expression)
DAV1D_OFFSETS = [
    ("Dav1dSettings", "n_threads", "n_threads"), ("Dav1dSettings", "max_frame_delay", "max_frame_delay"),
    ("Dav1dSettings", "apply_grain", "apply_grain"), ("Dav1dSettings", "operating_point", "operating_point"),
    ("Dav1dSettings", "all_layers", "all_layers"), ("Dav1dSettings", "frame_size_limit", "frame_size_limit"),
    ("Dav1dSettings", "allocator", "allocator"), ("Dav1dSettings", "logger_cookie", "logger.cookie"),
    ("Dav1dSettings", "logger_callback", "logger.callback"),
    ("Dav1dData", "data", "data"), ("Dav1dData", "size", "sz"),
    ("Dav1dSequenceHeader", "profile", "profile"), ("Dav1dSequenceHeader", "max_width", "max_width"),
    ("Dav1dSequenceHeader", "layout", "layout"), ("Dav1dSequenceHeader", "primaries", "pri"),
    ("Dav1dSequenceHeader", "transfer", "trc"), ("Dav1dSequenceHeader", "matrix", "mtrx"),
    ("Dav1dSequenceHeader", "chroma_position", "chr"), ("Dav1dSequenceHeader", "hbd", "hbd"),
    ("Dav1dSequenceHeader", "color_range", "color_range"),
    ("Dav1dPicture", "sequence", "seq_hdr"), ("Dav1dPicture", "frame_header", "frame_hdr"), ("Dav1dPicture", "data", "data"),
    ("Dav1dPicture", "stride", "stride"), ("Dav1dPicture", "width", "p.w"), ("Dav1dPicture", "height", "p.h"),
    ("Dav1dPicture", "layout", "p.layout"), ("Dav1dPicture", "bits_per_component", "p.bpc"),
]
OPENH264_STRUCTS = {"SBufferInfo": "SBufferInfo", "SDecodingParam": "SDecodingParam"}
OPENH264_OFFSETS = [
    ("SBufferInfo", "buffer_status", "iBufferStatus"), ("SBufferInfo", "in_timestamp", "uiInBsTimeStamp"),
    ("SBufferInfo", "out_timestamp", "uiOutYuvTimeStamp"), ("SBufferInfo", "width", "UsrData.sSystemBuffer.iWidth"),
    ("SBufferInfo", "height", "UsrData.sSystemBuffer.iHeight"), ("SBufferInfo", "format", "UsrData.sSystemBuffer.iFormat"),
    ("SBufferInfo", "stride", "UsrData.sSystemBuffer.iStride"), ("SBufferInfo", "planes", "pDst"),
    ("SDecodingParam", "file_name_restructed", "pFileNameRestructed"), ("SDecodingParam", "cpu_load", "uiCpuLoad"),
    ("SDecodingParam", "target_dq_layer", "uiTargetDqLayer"), ("SDecodingParam", "error_concealment", "eEcActiveIdc"),
    ("SDecodingParam", "parse_only", "bParseOnly"), ("SDecodingParam", "video_property_size", "sVideoProperty.size"),
    ("SDecodingParam", "bitstream_type", "sVideoProperty.eVideoBsType"),
]
# ISVCDecoderVtbl members in table order.
OPENH264_TABLE = ["Initialize", "Uninitialize", "DecodeFrame", "DecodeFrameNoDelay", "DecodeFrame2", "FlushFrame",
                  "DecodeParser", "DecodeFrameEx", "SetOption", "GetOption"]

FLAGS_CPP = ["-std=c++23", "-nostdinc", "-fms-extensions", "-fms-volatile", "-fshort-wchar", "-fno-exceptions", "-fno-rtti",
             "-Wno-invalid-offsetof", "-Wno-ignored-attributes", "-fsyntax-only"]


def run(command, check=True):
    result = subprocess.run(command, capture_output=True, text=True)
    if check and result.returncode:
        raise SystemExit("%s failed:\n%s%s" % (command[0], result.stdout, result.stderr))
    return result


def includes_text(headers):
    return "".join("#include <%s>\n" % h for h in headers)


def preprocess(cc, includes, headers):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "check.c")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write(includes_text(headers))
        return run([cc, "-E", "-P"] + ["-I" + d for d in includes] + [source]).stdout


def read_constants(path):
    text = open(path, encoding="utf-8").read()
    constants = []
    for match in re.finditer(r"inline constexpr (?P<type>[\w ]+?) k\w+ = (?P<value>[^;]+);\s*//\s*(?P<macro>[A-Za-z_]\w*)", text):
        constants.append((match["macro"], match["value"].strip()))
    return text, constants


def read_functions(text, table):
    body = text[text.index("#define " + table):]
    body = body[:body.index("\n\n")]
    rows = []
    for match in re.finditer(r"F\((?P<ret>[^,]+),\s*(?P<name>\w+),\s*\((?P<args>.*?)\)\)", body):
        rows.append((match["name"], match["ret"].strip(), split_arguments(match["args"])))
    return rows


def split_arguments(text):
    parts, depth, current = [], 0, ""
    for character in text:
        depth += {"(": 1, "[": 1, ")": -1, "]": -1}.get(character, 0)
        if character == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += character
    if current.strip():
        parts.append(current.strip())
    return [] if parts == ["void"] else parts


def kind(text):
    text = re.sub(r"\b(const|volatile|struct|enum|extern|DAV1D_API|EXTERN_C|EXPORT_API)\b", " ", text)
    text = " ".join(text.split())
    if "*" in text or text in POINTER_TYPEDEFS or text.endswith("]"):
        return "pointer"
    if text == "void":
        return "void"
    if text in SCALARS:
        return SCALARS[text]
    if re.fullmatch(r"(Dav1d|VA|DECODING)\w*(Type|State|Layout|Type|Flags|Mode|Frame)\w*", text):
        return 4  # an enumeration
    raise ValueError("cannot classify the type '%s'" % text)


def parameter_type(parameter):
    parameter = parameter.strip()
    if parameter.endswith("]"):
        return re.sub(r"\s*\w+\s*\[[^\]]*\]$", " *", parameter)
    match = re.match(r"^(.*?[\s\*])(\w+)$", parameter)
    if match and not re.match(r"^(void|int|long|unsigned|char|size_t|short)$", parameter):
        return match.group(1)
    return parameter


def find_declaration(text, name):
    for match in re.finditer(r"(?P<ret>[^;{}()]*?)\b%s\s*\(" % re.escape(name), text):
        start = match.end()
        depth, index = 1, start
        while index < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[index], 0)
            index += 1
        if depth:
            continue
        following = text[index:index + 64].lstrip()
        if not (following.startswith(";") or following.startswith("__attribute__")):
            continue
        ret = match["ret"].strip().split(";")[-1].strip()
        ret = re.sub(r"\b(extern|static|inline|__inline__|__attribute__\(\(.*?\)\))\b", " ", ret).strip()
        return ret, [parameter_type(p) for p in split_arguments(text[start:index - 1])]
    return None


def compare_function(text, name, ret, arguments):
    declared = find_declaration(text, name)
    if not declared:
        return "%s: not declared by the headers" % name
    declared_ret, declared_arguments = declared
    try:
        if len(declared_arguments) != len(arguments):
            return "%s: %d arguments declared, %d in the table" % (name, len(declared_arguments), len(arguments))
        if kind(declared_ret) != kind(ret):
            return "%s: returns %s, the table says %s" % (name, declared_ret, ret)
        for index, (theirs, ours) in enumerate(zip(declared_arguments, arguments)):
            if kind(theirs) != kind(ours):
                return "%s: argument %d is '%s', the table says '%s'" % (name, index + 1, theirs, ours)
    except ValueError as error:
        return "%s: %s" % (name, error)
    return None


def check_constants(cc, includes, headers, constants):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "constants.c")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("#include <stddef.h>\n#include <stdint.h>\n" + includes_text(headers))
            for macro, value in constants:
                handle.write("_Static_assert((unsigned long long)(%s) == (unsigned long long)(%s), \"%s\");\n"
                             % (macro, value, macro))
        result = run([cc, "-fsyntax-only"] + ["-I" + d for d in includes] + [source], check=False)
        if result.returncode:
            print(result.stderr)
            return 1
    return 0


def library_numbers(cc, includes, headers, expressions):
    """Evaluates C constant expressions with the real headers: {expression: value}."""
    with tempfile.TemporaryDirectory() as directory:
        source, binary = os.path.join(directory, "n.c"), os.path.join(directory, "n")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write("#include <stdio.h>\n#include <stddef.h>\n#include <stdint.h>\n" + includes_text(headers))
            handle.write("int main(void) {\n")
            for index, expression in enumerate(expressions):
                handle.write("    printf(\"%d %%zu\\n\", (size_t)(%s));\n" % (index, expression))
            handle.write("    return 0;\n}\n")
        run([cc, "-o", binary, source] + ["-I" + d for d in includes])
        values = {}
        for line in run([binary]).stdout.splitlines():
            index, value = line.split()
            values[expressions[int(index)]] = int(value)
        return values


def check_declarations(cxx, header, namespace, asserts):
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "layout.cpp")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write('#include "%s"\n' % os.path.relpath(header, os.path.join(ROOT, "include")).replace(os.sep, "/"))
            handle.write("using namespace maicord::%s;\n" % namespace)
            for expression, message in asserts:
                handle.write("static_assert(%s, \"%s\");\n" % (expression, message))
        command = [cxx] + FLAGS_CPP + ["-I" + os.path.join(ROOT, "include"), "-I" + os.path.join(ROOT, "include", "maicord", "sys", "libc"), source]
        result = run(command, check=False)
        if result.returncode:
            print(result.stderr)
            return 1
    return 0


def bit_fields(text):
    """Ordered (name, width) of the bit fields of a struct body, names normalized."""
    fields = []
    for match in re.finditer(r"(?:\w+\s+)*?(\w+)\s*:\s*(\d+)\s*;", text):
        fields.append((match[1].replace("_", "").lower(), int(match[2])))
    return fields


def struct_body(text, name, keyword):
    match = re.search(keyword % re.escape(name), text)
    if not match:
        return None
    depth, index = 1, text.index("{", match.start()) + 1
    start = index
    while index < len(text) and depth:
        depth += {"{": 1, "}": -1}.get(text[index], 0)
        index += 1
    return text[start:index - 1]


def check_bit_fields(va_text, header_text):
    failures = 0
    for mine, theirs in VA_STRUCTS.items():
        ours = struct_body(header_text, mine, r"struct %s\s*{")
        real = struct_body(va_text, theirs, r"typedef struct _?%s\b[^{]*{") or struct_body(va_text, theirs, r"struct _%s\s*{")
        if ours is None or real is None:
            print("%s: no declaration to compare the bit fields of" % mine)
            failures += 1
            continue
        a, b = bit_fields(ours), bit_fields(real)
        # The library's own reserved members are named differently; widths and order are what matter.
        if [w for _, w in a] != [w for _, w in b] or [n for n, _ in a if n != "reserved" and not n.startswith("reserved")] != [
                n for n, _ in b if n != "reserved" and not n.startswith("reserved")]:
            print("%s: bit fields differ from %s:\n  ours   %s\n  theirs %s" % (mine, theirs, a, b))
            failures += 1
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--compiler", default=os.environ.get("CC") or shutil.which("cc") and "cc" or "clang")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    arguments = parser.parse_args()
    includes = arguments.include or ["/usr/include"]
    failures, checked = 0, 0

    # ---- VA-API ----
    va_header, va_constants = read_constants(VA_HEADER)
    failures += check_constants(arguments.compiler, includes, VA_INCLUDES, va_constants)
    va_text = preprocess(arguments.compiler, includes, VA_INCLUDES + ("va/va_drm.h",))
    expressions = ["sizeof(%s)" % real for real in VA_STRUCTS.values()]
    expressions += ["offsetof(%s, %s)" % (VA_STRUCTS[s], VA_FIELDS.get((s, f), f)) for s, f in VA_OFFSETS]
    numbers = library_numbers(arguments.compiler, includes, VA_INCLUDES, expressions)
    asserts = [("sizeof(%s) == %d" % (mine, numbers["sizeof(%s)" % real]), "sizeof(%s)" % real) for mine, real in VA_STRUCTS.items()]
    asserts += [("__builtin_offsetof(%s, %s) == %d" % (s, f, numbers["offsetof(%s, %s)" % (VA_STRUCTS[s], VA_FIELDS.get((s, f), f))]),
                 "offsetof(%s, %s)" % (s, f)) for s, f in VA_OFFSETS]
    failures += check_declarations(arguments.cxx, VA_HEADER, "vaapi", asserts)
    failures += check_bit_fields(va_text, va_header)
    for table in ("MAICORD_VA_FUNCTIONS", "MAICORD_VA_DRM_FUNCTIONS"):
        for name, ret, table_arguments in read_functions(va_header, table):
            problem = compare_function(va_text, name, ret, table_arguments)
            checked += 1
            if problem:
                failures += 1
                print(problem)
    print("VA-API: %d constants, %d layouts and %d functions checked" % (len(va_constants), len(asserts), checked))

    # ---- libdav1d ----
    software_header, software_constants = read_constants(SOFTWARE_HEADER)
    dav1d_constants = [c for c in software_constants if c[0].startswith(("DAV1D_", "EAGAIN"))]
    failures += check_constants(arguments.compiler, includes, DAV1D_INCLUDES, dav1d_constants)
    expressions = ["sizeof(%s)" % real for real in DAV1D_STRUCTS.values()]
    expressions += ["offsetof(%s, %s)" % (s, e) for s, _, e in DAV1D_OFFSETS]
    numbers = library_numbers(arguments.compiler, includes, DAV1D_INCLUDES, expressions)
    # The declared structs keep the leading fields at the library's offsets and room for the rest.
    # (Dav1dSequenceHeader is only ever read through the library's pointer: its tail is not declared.)
    asserts = [("sizeof(%s) >= %d" % (mine, numbers["sizeof(%s)" % real]), "room for sizeof(%s)" % real)
               for mine, real in DAV1D_STRUCTS.items() if mine != "Dav1dSequenceHeader"]
    asserts += [("__builtin_offsetof(%s, %s) == %d" % (s, f, numbers["offsetof(%s, %s)" % (s, e)]), "offsetof(%s, %s)" % (s, f))
                for s, f, e in DAV1D_OFFSETS]
    failures += check_declarations(arguments.cxx, SOFTWARE_HEADER, "software", asserts)
    dav1d_text = preprocess(arguments.compiler, includes, DAV1D_INCLUDES)
    count = 0
    for name, ret, table_arguments in read_functions(software_header, "MAICORD_DAV1D_FUNCTIONS"):
        problem = compare_function(dav1d_text, name, ret, table_arguments)
        count += 1
        if problem:
            failures += 1
            print(problem)
    print("libdav1d: %d constants, %d layouts and %d functions checked" % (len(dav1d_constants), len(asserts), count))

    # ---- libopenh264 ----
    openh264_constants = [c for c in software_constants if not c[0].startswith(("DAV1D_", "EAGAIN"))]
    failures += check_constants(arguments.compiler, includes, OPENH264_INCLUDES, openh264_constants)
    expressions = ["sizeof(%s)" % real for real in OPENH264_STRUCTS.values()]
    expressions += ["offsetof(%s, %s)" % (s, e) for s, _, e in OPENH264_OFFSETS]
    expressions += ["offsetof(ISVCDecoderVtbl, %s)" % member for member in OPENH264_TABLE]
    numbers = library_numbers(arguments.compiler, includes, OPENH264_INCLUDES, expressions)
    asserts = [("sizeof(%s) == %d" % (mine, numbers["sizeof(%s)" % real]), "sizeof(%s)" % real)
               for mine, real in OPENH264_STRUCTS.items()]
    asserts += [("__builtin_offsetof(%s, %s) == %d" % (s, f, numbers["offsetof(%s, %s)" % (s, e)]), "offsetof(%s, %s)" % (s, f))
                for s, f, e in OPENH264_OFFSETS]
    asserts += [("__builtin_offsetof(IsvcDecoderTable, %s) == %d" % (m, numbers["offsetof(ISVCDecoderVtbl, %s)" % m]),
                 "ISVCDecoderVtbl.%s" % m) for m in OPENH264_TABLE]
    failures += check_declarations(arguments.cxx, SOFTWARE_HEADER, "software", asserts)
    openh264_text = preprocess(arguments.compiler, includes, OPENH264_INCLUDES)
    for name, ret, table_arguments in (("WelsCreateDecoder", "long", ["ISVCDecoder**"]),
                                       ("WelsDestroyDecoder", "void", ["ISVCDecoder*"])):
        problem = compare_function(openh264_text, name, ret, table_arguments)
        if problem:
            failures += 1
            print(problem)
    # The table's member signatures: argument counts and kinds, in the library's own spelling.
    vtable = struct_body(openh264_text, "ISVCDecoderVtbl", r"struct %s\s*{")
    ours = {}
    for match in re.finditer(r"^\s*(?P<ret>[\w ]+?)\s*\(\*(?P<name>\w+)\)\s*\((?P<args>[^;]*)\);", software_header, re.M):
        ours[match["name"]] = (match["ret"], split_arguments(match["args"]))
    for match in re.finditer(r"(?P<ret>[\w ]+?)\s*\(\*\s*(?P<name>\w+)\)\s*\((?P<args>.*?)\)\s*;", vtable or "", re.S):
        name = match["name"]
        if name not in ours:
            continue
        theirs = [parameter_type(a) for a in split_arguments(match["args"])]
        try:
            same = len(theirs) == len(ours[name][1]) and kind(match["ret"].strip()) == kind(ours[name][0]) and all(
                kind(a) == kind(b) for a, b in zip(theirs, ours[name][1]))
        except ValueError as error:
            print("%s: %s" % (name, error))
            same = False
        if not same:
            failures += 1
            print("ISVCDecoderVtbl.%s: %s vs %s" % (name, theirs, ours[name][1]))
    print("libopenh264: %d constants and %d layouts checked" % (len(openh264_constants), len(asserts)))
    print("video ABI: %s" % ("FAILED" if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
