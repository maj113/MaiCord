bool native_font_selection_cases() noexcept {
    NativeTextCatalogs catalogs{};NativeFontSession* session=nullptr;Arena scratch{};NativeFontInstanceHandle instance{};
    struct Cleanup {NativeTextCatalogs& c;NativeFontSession*& s;Arena& a;NativeFontInstanceHandle& instance;
        ~Cleanup() noexcept {native_text_catalogs_release(&c);if(s){if(instance.slot)native_font_instance_release(native_font_session_instances(s),instance);native_font_session_release(&s);}arena_release(&a);}
    } cleanup{catalogs,session,scratch,instance};
    CHECK(native_font_session_create(&session)&&arena_initialize(&scratch,1024*1024,0,4096)&&native_text_catalogs_initialize(&catalogs));
    const auto query=[](const wchar_t* name) noexcept {u32 n=0;while(name[n])++n;return NativeTextFontQuery{0,{reinterpret_cast<const u16*>(name),n},{400,5,0}};};
    const auto base=query(L"Segoe UI");NativeTextFontRange range{};NativeFontSelection source{};
    CHECK(!catalogs.files&&!catalogs.storage.committed);
    CHECK(native_text_catalog_range(&catalogs,base,session,&scratch,utf8_literal("ABC"),&range)&&range.bytes==3);
    CHECK(catalogs.files&&!catalogs.files->next&&catalogs.files->catalog.count.files==1);
    const auto selected=range.match.candidate;auto* first=catalogs.files;const auto saved=first->catalog.candidates[selected];
    CHECK(native_text_catalog_select_face(&catalogs,0,selected,0,session,&scratch,&instance,&source));
    const auto* held=native_font_instance_get(*native_font_session_instances(session),instance);CHECK(held);
    const auto* face=native_font_get(*native_font_session_fonts(session),held->face);CHECK(face);u16 glyph=0;
    CHECK(sfnt_cmap_glyph(face->cmap,'A',0,&glyph)==SfntStatus::ok&&glyph);
    const auto path=native_font_catalog_path(first->catalog,0);u32 scope=0;
    CHECK(native_text_catalog_register_path(&catalogs,reinterpret_cast<const wchar_t*>(path.data),&scope)&&scope);
    auto private_query=base;private_query.scope=scope;
    CHECK(native_text_catalog_range(&catalogs,private_query,session,&scratch,utf8_literal("ABC"),&range)&&range.scope==scope);
    const Utf8View samples[]{utf8_literal("日本語"),utf8_literal("한국어"),utf8_literal("नमस्ते"),utf8_literal("ภาษาไทย"),utf8_literal("אב")};
    u32 candidates[5]{};u64 builds=0;usize offset=0;
    for(u32 pass=0;pass<2;++pass){for(u32 i=0;i<5;++i){auto request=base;if(i==4)request.properties={600,5,2};
            CHECK(native_text_catalog_range(&catalogs,request,session,&scratch,samples[i],&range));
            CHECK(range.bytes==samples[i].bytes&&range.scope==0&&range.match.candidate!=native_fallback_missing);
            if(!pass)candidates[i]=range.match.candidate;else CHECK(range.match.candidate==candidates[i]);
        }if(!pass){builds=catalogs.builds;offset=catalogs.storage.offset;}else CHECK(catalogs.builds==builds&&catalogs.storage.offset==offset);
    }
    CHECK(bytes_equal(&saved,&first->catalog.candidates[selected],sizeof(saved)));
    CHECK(native_text_catalog_range(&catalogs,private_query,session,&scratch,utf8_literal("ABC"),&range)&&range.scope==scope);
    auto variable=query(L"Bahnschrift");
    for(u32 weight=400;weight<=600;weight+=200){variable.properties.weight=static_cast<u16>(weight);NativeFontInstanceHandle value{};u32 simulations=99;
        CHECK(native_text_catalog_select_query(&catalogs,variable,session,&scratch,&value,&source,nullptr,&simulations));
        CHECK(value.slot&&source.count&&!simulations);CHECK(native_font_instance_release(native_font_session_instances(session),value));
    }
    const auto missing=query(L"MaiCord QA absent family 8F41");const auto reserved=catalogs.storage.reserved;auto* before=catalogs.files;
    catalogs.storage.reserved=catalogs.storage.offset;NativeFontInstanceHandle absent{};
    const bool failed=native_text_catalog_select_query(&catalogs,missing,session,&scratch,&absent,&source);
    catalogs.storage.reserved=reserved;
    CHECK(!failed&&!absent.slot&&catalogs.files==before);
    CHECK(native_text_catalog_range(&catalogs,base,session,&scratch,utf8_literal("ABC"),&range)&&range.match.candidate==selected);
    CHECK(!native_text_catalog_select_query(&catalogs,missing,session,&scratch,&absent,&source));
    auto* cached=catalogs.families;const auto previous_builds=catalogs.builds;
    CHECK(!native_text_catalog_select_query(&catalogs,missing,session,&scratch,&absent,&source)&&catalogs.families==cached&&catalogs.builds==previous_builds&&catalogs.files==before);
    u32 chosen=99;bool missing_family=false;
    CHECK(native_text_catalog_select_query(&catalogs,missing,session,&scratch,&absent,&source,nullptr,nullptr,&chosen,&missing_family));
    CHECK(absent.slot&&missing_family&&!chosen);CHECK(native_font_instance_release(native_font_session_instances(session),absent));
    auto invalid=missing;invalid.properties.weight=0;
    CHECK(!native_text_catalog_select_query(&catalogs,invalid,session,&scratch,&absent,&source)&&!absent.slot);
    native_text_catalogs_release(&catalogs);
    CHECK(native_font_instance_get(*native_font_session_instances(session),instance)==held&&sfnt_cmap_glyph(face->cmap,'A',0,&glyph)==SfntStatus::ok&&glyph);
    print("font_selection_lazy_scripts_styles_variable_private_missing_failure_lifetime_passed\n");return true;
}
