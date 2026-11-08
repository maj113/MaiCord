// DirectWrite glyph outlines and face binding for MaiCordFontReference; see dwrite_outline_reference.hpp.
#include "dwrite_outline_reference.hpp"
#include "maicord/text/fonts/session.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"
#include "maicord/sys/d2d1.hpp"

namespace maicord {
namespace {

bool interface_equal(REFIID left, REFIID right) noexcept {
    const auto* left_bytes = reinterpret_cast<const volatile u8*>(&left);
    const auto* right_bytes = reinterpret_cast<const volatile u8*>(&right);
    bool equal = true;
    for (u32 index = 0; index < sizeof(IID); ++index) {
        equal = equal && left_bytes[index] == right_bytes[index];
    }
    return equal;
}

class GlyphOutlineSink final : public ID2D1SimplifiedGeometrySink {
  public:
    explicit GlyphOutlineSink(GlyphOutline* output) noexcept
        : references_(1), output_(output), figure_open_(false), overflowed_(false),
          invalid_sequence_(false) {
    }

    bool overflowed() const noexcept {
        return overflowed_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interface_id, void** object) noexcept override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_equal(interface_id, __uuidof(IUnknown)) ||
            interface_equal(interface_id, __uuidof(ID2D1SimplifiedGeometrySink))) {
            *object = static_cast<ID2D1SimplifiedGeometrySink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return static_cast<ULONG>(InterlockedDecrement(&references_));
    }

    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE fill_mode) noexcept override {
        if (!output_) {
            invalid_sequence_ = true;
            return;
        }
        output_->fill_rule =
            fill_mode == D2D1_FILL_MODE_WINDING ? GlyphFillRule::winding : GlyphFillRule::alternate;
    }

    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {
    }

    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F point, D2D1_FIGURE_BEGIN begin) noexcept override {
        if (figure_open_) {
            invalid_sequence_ = true;
            return;
        }
        figure_open_ = true;
        const GlyphPathCommandKind kind = begin == D2D1_FIGURE_BEGIN_HOLLOW
                                              ? GlyphPathCommandKind::begin_hollow
                                              : GlyphPathCommandKind::begin_filled;
        append(GlyphPathCommand{point.x, point.y, 0.0f, 0.0f, 0.0f, 0.0f, kind, {0, 0, 0}}, 1);
    }

    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) noexcept override {
        if (!figure_open_ || (!points && count != 0)) {
            invalid_sequence_ = true;
            return;
        }
        for (u32 index = 0; index < count; ++index) {
            append(GlyphPathCommand{points[index].x, points[index].y, 0.0f, 0.0f, 0.0f, 0.0f,
                                    GlyphPathCommandKind::line, {0, 0, 0}},
                   1);
        }
    }

    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 count) noexcept override {
        if (!figure_open_ || (!beziers && count != 0)) {
            invalid_sequence_ = true;
            return;
        }
        for (u32 index = 0; index < count; ++index) {
            const D2D1_BEZIER_SEGMENT& curve = beziers[index];
            append(GlyphPathCommand{curve.point1.x, curve.point1.y, curve.point2.x, curve.point2.y, curve.point3.x,
                                    curve.point3.y, GlyphPathCommandKind::cubic, {0, 0, 0}},
                   3);
        }
    }

    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) noexcept override {
        if (!figure_open_) {
            invalid_sequence_ = true;
            return;
        }
        const GlyphPathCommandKind kind = end == D2D1_FIGURE_END_CLOSED ? GlyphPathCommandKind::end_closed
                                                                        : GlyphPathCommandKind::end_open;
        append(GlyphPathCommand{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, kind, {0, 0, 0}}, 0);
        figure_open_ = false;
    }

    HRESULT STDMETHODCALLTYPE Close() noexcept override {
        if (overflowed_) {
            return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        return figure_open_ || invalid_sequence_ ? E_UNEXPECTED : S_OK;
    }

  private:
    void include_point(float x, float y) noexcept {
        if (!output_->has_bounds) {
            output_->minimum_x = x;
            output_->maximum_x = x;
            output_->minimum_y = y;
            output_->maximum_y = y;
            output_->has_bounds = true;
            return;
        }
        output_->minimum_x = minimum(output_->minimum_x, x);
        output_->minimum_y = minimum(output_->minimum_y, y);
        output_->maximum_x = maximum(output_->maximum_x, x);
        output_->maximum_y = maximum(output_->maximum_y, y);
    }

    void append(const GlyphPathCommand& command, u32 point_count) noexcept {
        if (!output_ || overflowed_) {
            return;
        }
        if (output_->count >= output_->capacity) {
            overflowed_ = true;
            ++output_->rejected;
            return;
        }
        output_->commands[output_->count++] = command;
        if (point_count >= 1) {
            include_point(command.x1, command.y1);
        }
        if (point_count >= 2) {
            include_point(command.x2, command.y2);
        }
        if (point_count >= 3) {
            include_point(command.x3, command.y3);
        }
    }

    volatile LONG references_;
    GlyphOutline* output_;
    bool figure_open_;
    bool overflowed_;
    bool invalid_sequence_;
};

template<class T> Slice<T> take(Arena& arena,u32 count) noexcept {return {count?arena_allocate<T>(&arena,count):nullptr,count};}
NativeFontResult failure(SfntStatus status) noexcept {return {NativeFontError::font,status,0};}
NativeFontResult windows_error(HRESULT hr) noexcept {return {NativeFontError::io,SfntStatus::ok,static_cast<u32>(hr)};}

} // namespace

GlyphOutlineResult text_outline_glyph(IDWriteFontFace* font_face, u16 glyph_index,
                                      GlyphOutline* output) noexcept {
    if (!font_face || !output || !output->commands || output->capacity == 0) {
        return GlyphOutlineResult{E_INVALIDARG, 0, GlyphOutlineError::invalid_input};
    }
    glyph_outline_reset(output);
    GlyphOutlineSink sink(output);
    const HRESULT outlined =
        font_face->GetGlyphRunOutline(1.0f, &glyph_index, nullptr, nullptr, 1, FALSE, FALSE, &sink);
    const HRESULT closed = sink.Close();
    const HRESULT result = FAILED(outlined) ? outlined : closed;
    if (FAILED(result)) {
        const GlyphOutlineError error =
            sink.overflowed() || result == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)
                ? GlyphOutlineError::output_too_small
                : GlyphOutlineError::directwrite;
        output->count = 0;
        output->has_bounds = false;
        return GlyphOutlineResult{result, 0, error};
    }
    return GlyphOutlineResult{S_OK, output->count, GlyphOutlineError::none};
}

NativeFontResult native_font_bind_selected(NativeFontSession* session,IDWriteFontFace* selected,bool outline_instructions,Arena* scratch,NativeFontInstanceHandle* output,bool* oblique,bool* bold,NativeFontSelection* source) noexcept {
    if(source)*source={};
    if(oblique)*oblique=false;if(bold)*bold=false;if(output)*output={};if(!session||!selected||!scratch||!scratch->base||!output)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto simulations=selected->GetSimulations();
    if(static_cast<u32>(simulations)>3||((simulations&DWRITE_FONT_SIMULATIONS_OBLIQUE)&&!oblique)||((simulations&DWRITE_FONT_SIMULATIONS_BOLD)&&!bold))return failure(SfntStatus::unsupported);
    u32 count=0;HRESULT hr=selected->GetFiles(&count,nullptr);if(FAILED(hr))return windows_error(hr);if(count!=1)return failure(SfntStatus::unsupported);
    IDWriteFontFile* file=nullptr;IDWriteFontFileLoader* loader=nullptr;IDWriteLocalFontFileLoader* local=nullptr;IDWriteFontFace5* variable=nullptr;
    struct Release {IDWriteFontFile*& f;IDWriteFontFileLoader*& l;IDWriteLocalFontFileLoader*& p;IDWriteFontFace5*& v;~Release() noexcept {com_release(v);com_release(p);com_release(l);com_release(f);}} release{file,loader,local,variable};
    hr=selected->GetFiles(&count,&file);const void* key=nullptr;u32 key_size=0,length=0;
    if(SUCCEEDED(hr))hr=file->GetReferenceKey(&key,&key_size);
    if(SUCCEEDED(hr))hr=file->GetLoader(&loader);
    if(SUCCEEDED(hr))hr=loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local));
    if(SUCCEEDED(hr))hr=local->GetFilePathLengthFromKey(key,key_size,&length);
    if(FAILED(hr))return windows_error(hr);if(!length||length>32767)return failure(SfntStatus::unsupported);
    arena_reset(scratch);auto path=take<wchar_t>(*scratch,length+1);if(!path.data)return failure(SfntStatus::output_too_small);
    hr=local->GetFilePathFromKey(key,key_size,path.data,path.count);if(FAILED(hr))return windows_error(hr);
    NativeFontHandle face{};const auto acquired=native_font_session_face(session,path.data,selected->GetIndex(),&face);if(!acquired.ok())return acquired;
    auto& fonts=*native_font_session_fonts(session);
    struct ReleaseFace {NativeFontStore& s;NativeFontHandle h;~ReleaseFace() noexcept {native_font_release(&s,h);}} release_face{fonts,face};
    const auto& font=native_font_get(fonts,face)->geometry;ConstSlice<FontAxisSetting> settings{};
    if((simulations&DWRITE_FONT_SIMULATIONS_BOLD)&&(font.kind!=FontGeometryKind::truetype||font.gvar.table.data))return failure(SfntStatus::unsupported);
    if(font.axes.axis_count){
        hr=selected->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&variable));if(FAILED(hr))return windows_error(hr);
        const u32 axes=variable->GetFontAxisValueCount();if(axes>64)return failure(SfntStatus::unsupported);
        auto values=take<DWRITE_FONT_AXIS_VALUE>(*scratch,axes);auto native=take<FontAxisSetting>(*scratch,axes);
        if(!values.data||!native.data)return failure(SfntStatus::output_too_small);hr=variable->GetFontAxisValues(values.data,axes);if(FAILED(hr))return windows_error(hr);
        for(u32 i=0;i<axes;++i){const u32 t=values[i].axisTag;native[i]={((t&255)<<24)|((t&0xff00)<<8)|((t>>8)&0xff00)|(t>>24),values[i].value};}settings={native.data,axes};
    }
    const auto result=native_font_prepare(session,face,settings,static_cast<u32>(simulations),outline_instructions,scratch,output,oblique,bold);
    if(result.ok()&&source&&font.axes.axis_count<=2){NativeFontSelection saved{};saved.face=face;saved.count=font.axes.axis_count;
        for(u32 i=0;i<saved.count;++i){FontAxis axis{};font_axis(font.axes,i,&axis);saved.settings[i]={axis.tag,axis.normal/65536.0};
            for(u32 j=0;j<settings.count;++j)if(settings[j].tag==axis.tag)saved.settings[i]=settings[j];}*source=saved;}
    return result;
}

} // namespace maicord
