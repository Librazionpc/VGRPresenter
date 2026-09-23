#include "modules/library/DesignCatalogs.hpp"

#include <format>

namespace bps::library {

namespace {

namespace pres = presentation;

// FreeShow lays its overlays out on a 1920 x 1080 screen; every number below is written the way FreeShow has it and
// scaled onto the Edit screen's stage here, so the shipped overlays can be checked against the original.
constexpr double kSx = kStageWidth / 1920.0;
constexpr double kSy = kStageHeight / 1080.0;

// A block at FreeShow's screen position. `id` is set by Numbered() once the list is complete.
pres::ContentBlock At(std::string kind, double x, double y, double w, double h) {
    pres::ContentBlock b;
    b.kind = std::move(kind);
    b.x = x * kSx;
    b.y = y * kSy;
    b.width = w * kSx;
    b.height = h * kSy;
    return b;
}

pres::ContentBlock Shape(const char* shapeType, double x, double y, double w, double h, std::string fill) {
    pres::ContentBlock b = At("shape", x, y, w, h);
    b.style.backgroundColor = std::move(fill);
    b.metaJson = std::format(R"({{"shapeType":"{}"}})", shapeType);
    return b;
}

pres::ContentBlock Outline(pres::ContentBlock b, double width, std::string color) {
    b.style.borderEnabled = true;
    b.style.borderWidth = width;
    b.style.borderColor = std::move(color);
    return b;
}

pres::ContentBlock Text(double x, double y, double w, double h, std::string text, double fontSize,
                        std::string background = "transparent", bool bold = false, const char* align = "left",
                        const char* fontFamily = "Segoe UI", std::string bind = {}, std::string color = "#ffffff",
                        bool italic = false, const char* textCase = "none", const char* verticalAlign = "center") {
    pres::ContentBlock b = At("text", x, y, w, h);
    b.text = std::move(text);
    b.style.backgroundColor = std::move(background);
    b.style.padding = 4;
    b.bind = std::move(bind);
    // autoSize "shrink": the set size is a CEILING, not a fixed value - DesignPreview/the canvas only shrinks
    // the font (down to a 3px floor) when the box is too small for it, never growing past what's set here. Was
    // hardcoded "none" (fixed size + elide), which silently truncated any bound text longer than the block was
    // sized for - a sermon paragraph or a long verse would just cut off mid-sentence with "…" instead of fitting.
    b.metaJson = std::format(
        R"({{"color":"{}","fontFamily":"{}","fontSize":{:.1f},"bold":{},"italic":{},"align":"{}","autoSize":"shrink","textCase":"{}","verticalAlign":"{}"}})",
        color, fontFamily, fontSize * kSy, bold ? "true" : "false", italic ? "true" : "false", align, textCase, verticalAlign);
    return b;
}

// A filled, rounded, optionally bordered box (FreeShow's decorative background items).
pres::ContentBlock Box(double x, double y, double w, double h, std::string fill, double radius = 0,
                       double borderWidth = 0, const char* borderColor = "#ffffff") {
    pres::ContentBlock b = Shape("rectangle", x, y, w, h, std::move(fill));
    b.style.cornerRadius = radius * kSx;
    if (borderWidth > 0) return Outline(std::move(b), borderWidth * kSx, borderColor);
    return b;
}

// A whole-screen treatment (vignette / corners): `inset` is FreeShow's pixel value.
pres::ContentBlock Screen(const char* kind, std::string color, double inset) {
    pres::ContentBlock b = At(kind, 0, 0, 1920, 1080);
    b.style.backgroundColor = std::move(color);
    b.metaJson = std::format(R"({{"inset":{:.1f}}})", inset * kSx);
    return b;
}

std::vector<pres::ContentBlock> Numbered(std::vector<pres::ContentBlock> blocks) {
    int n = 1;
    for (pres::ContentBlock& b : blocks) b.id = std::format("item-{}", n++);
    return blocks;
}

Design Overlay(std::string id, std::string name, std::string color, std::vector<pres::ContentBlock> blocks) {
    Design d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.color = std::move(color);
    d.category = "visuals";
    d.isDefault = true;
    d.blocks = Numbered(std::move(blocks));
    return d;
}

std::vector<Design> ShippedOverlays() {
    std::vector<Design> out;

    // A white frame around the picture, a red dot and "REC".
    {
        pres::ContentBlock frame = Outline(Shape("rectangle", 36.5, 35, 1847.6, 1008.2, "transparent"), 1.6, "#ffffff");
        out.push_back(Overlay("recording", "Recording", "red",
                              { frame, Shape("circle", 80, 80, 40, 40, "#ff0000"), Text(140, 80, 100, 40, "REC", 40) }));
    }

    // The time, top right.
    {
        pres::ContentBlock clock = At("clock", 1450, 70, 470, 150);
        clock.style.backgroundColor = "#99000000";
        clock.style.cornerRadius = 6;
        clock.metaJson = R"({"format":"24","style":"digital","showSeconds":false,"showDate":false,"color":"#ffffff"})";
        out.push_back(Overlay("clock", "Clock", "dodgerblue", { clock }));
    }

    // A round analog clock in the middle.
    {
        pres::ContentBlock face = At("clock", 492, 72.5, 936.4, 936.4);
        face.width = face.height;                    // round: as wide as it is tall on the stage
        face.x = (kStageWidth - face.width) / 2;
        face.style.backgroundColor = "#80000000";
        face.style.cornerRadius = face.width / 2;
        face.style.borderEnabled = true;
        face.style.borderWidth = 1;
        face.style.borderColor = "#ffffff";
        face.metaJson = R"({"format":"24","style":"analog","showSeconds":true,"showDate":false,"color":"#ffffff"})";
        out.push_back(Overlay("clock_analog", "Clock (Analog)", "dodgerblue", { face }));
    }

    // A name lower third: the bar, an accent stripe, the name and a title above it. Leaves by itself after 4 s.
    {
        Design name = Overlay("name", "Name", "#0b57a2",
                              { Shape("rectangle", 80, 875, 750, 135, "#0b57a2"),
                                Shape("rectangle", 80, 875, 50, 135, "#74cbfb"),
                                Text(130, 935, 700, 75, "Name Surname", 70, "#0b57a2", false, "left", "Arial"),
                                Text(130, 875, 700, 60, "Title", 40, "#006fcf", true, "left", "Arial", "", "#ffffff", false, "upper") });
        name.displayDuration = 4;
        out.push_back(std::move(name));
    }

    // The screen's four corners rounded off.
    {
        Design rounded = Overlay("rounded", "Rounded", {}, { Screen("corners", "#000000", 50) });
        rounded.locked = true;
        out.push_back(std::move(rounded));
    }

    // The screen's edges softened.
    {
        Design vignette = Overlay("vignette", "Vignette", "#dddddd", { Screen("vignette", "#ffffff", 248) });
        vignette.locked = true;
        out.push_back(std::move(vignette));
    }
    return out;
}

// A new overlay starts as a lower third the user can restyle: a bar with a line of text.
std::vector<pres::ContentBlock> OverlayStarter() {
    return { Shape("rectangle", 80, 900, 12, 100, "#74cbfb"),
             Text(92, 900, 760, 100, "Text", 64, "#0b57a2") };
}

// -----------------------------------------------------------------------------
// The shipped TEMPLATES - FreeShow's own starter set (createData.ts
// getDefaultTemplates / getDefaultScriptureTemplates / createDoubleTemplate), copied over like the
// overlays above so the app opens with a usable library instead of a mock one. FreeShow styles every
// template item on a 1920 x 1080 screen; the At() helper scales those numbers onto the stage, so the
// layout below keeps the original's proportions.
// -----------------------------------------------------------------------------

Design Template(std::string id, std::string name, std::string category, std::string color,
                std::vector<pres::ContentBlock> blocks) {
    Design d;
    d.id = "tpl-" + id;
    d.name = std::move(name);
    d.color = std::move(color);
    d.category = std::move(category);
    d.isDefault = true;
    d.contentType = d.category == "presentation" ? "notes" : d.category;   // the show category kind it fits
    // A template only shows a slide's content through `bind`. A layout with exactly one free-standing text (the
    // usual FreeShow template) shows the slide's text there; layouts with several texts bind them explicitly.
    pres::ContentBlock* free = nullptr;
    int freeCount = 0;
    for (pres::ContentBlock& b : blocks)
        if (b.kind == "text" && b.bind.empty() && b.text.find('{') == std::string::npos) { free = &b; ++freeCount; }
    if (freeCount == 1) free->bind = "text";
    d.blocks = Numbered(std::move(blocks));
    return d;
}

// The grey of a text-only template (FreeShow gives text templates color null and lets the text carry it;
// the card accent uses this so the chip still reads).
constexpr const char* kTextAccent = "#747680";
constexpr const char* kScriptureAccent = "#876543";
constexpr const char* kTableAccent = "#7a5fb8";

std::vector<Design> ShippedTemplates() {
    std::vector<Design> out;

    // ---- song ----
    // Plain centered defaults (both axes) - left pickable, not baked in: the Edit screen's TextItemPanel now
    // has a real vertical-align row (top/center/bottom) alongside the existing horizontal one, so whichever
    // template this text box is reused for, someone can choose top/justify (or anything else) per item from
    // the canvas instead of the engine forcing one choice on every user of this shape.
    out.push_back(Template("big", "Big", "song", kTextAccent,
        { Text(50, 88, 1820, 904, "Big", 120, "transparent", false, "center") }));
    out.push_back(Template("default", "Default", "song", kTextAccent, { Text(50, 88, 1820, 904, "Default", 100, "transparent", false, "center") }));
    out.push_back(Template("small", "Small", "song", kTextAccent, { Text(50, 88, 1820, 904, "Small", 80, "transparent", false, "center") }));
    out.push_back(Template("big_bold", "Big Bold", "song", kTextAccent, { Text(50, 88, 1820, 904, "Big Bold", 120, "transparent", true, "center") }));
    out.push_back(Template("default_bold", "Default Bold", "song", kTextAccent, { Text(50, 88, 1820, 904, "Default Bold", 100, "transparent", true, "center") }));
    out.push_back(Template("small_bold", "Small Bold", "song", kTextAccent, { Text(50, 88, 1820, 904, "Small Bold", 80, "transparent", true, "center") }));
    out.push_back(Template("double", "Double", "song", kTextAccent,
                           { Text(30, 550, 1860, 500, "2", 80, "transparent", false, "center", "Segoe UI", "line2", "#dddddd"),
                             Text(30, 30, 1860, 500, "1", 80, "transparent", false, "left", "Segoe UI", "line1") }));

    // A dark translucent band with numbered lines (FreeShow's "Blur box" without the backdrop blur).
    {
        std::vector<pres::ContentBlock> blur = { Box(0, 310, 1920, 460, "#80000000", 0),
                                                 Text(80, 330, 1760, 420, "1\n2\n3\n4", 90) };
        out.push_back(Template("blur_box", "Blur box", "song", kTextAccent, std::move(blur)));
    }

    // A dark screen with a clear band for the lines.
    {
        std::vector<pres::ContentBlock> faded = { Box(0, 0, 1920, 1080, "#80000000"),
                                                  Box(0, 310, 1920, 460, "transparent"),
                                                  Text(80, 330, 1760, 420, "1\n2\n3\n4", 90) };
        out.push_back(Template("faded", "Faded", "song", kTextAccent, std::move(faded)));
    }

    // A rounded box around the lines.
    {
        std::vector<pres::ContentBlock> box = { Box(51, 387.5, 1820, 307, "#66000000", 20, 8),
                                                Text(120, 410, 1680, 270, "1\n2\n3", 80, "transparent", true) };
        out.push_back(Template("box", "Box", "song", kTextAccent, std::move(box)));
    }

    // Lines on dark bars, one bar per line (FreeShow's "Trendy curved", simplified to rectangles).
    {
        std::vector<pres::ContentBlock> trendy;
        for (int i = 0; i < 2; ++i)
            trendy.push_back(Box(140, 360 + i * 190, 1640, 150, "#000000", 30));
        trendy.push_back(Text(200, 370, 1520, 330, "1\n2", 100));
        out.push_back(Template("trendy_curved", "Trendy Curved", "song", kTextAccent, std::move(trendy)));
    }

    // Lines over a soft horizontal fade.
    {
        std::vector<pres::ContentBlock> fade = { Screen("vignette", "#80000000", 260),
                                                 Text(80, 250, 1760, 580, "1\n2\n3\n4", 100) };
        out.push_back(Template("fade", "Fade", "song", kTextAccent, std::move(fade)));
    }

    // ---- song: lower thirds (purple accent in FreeShow) ----
    out.push_back(Template("lower_third", "Lower Third", "song", "#800080",
                           { Text(50, 820, 1820, 220, "1", 70, "transparent", true, "left") }));
    out.push_back(Template("lower_third_white", "Lower Third White", "song", "#800080",
                           { Box(50, 820, 1820, 220, "#ffffff", 20, 5),
                             Text(75, 830, 1770, 200, "1", 70, "transparent", true, "left", "Segoe UI", "", "#000000") }));
    out.push_back(Template("lower_third_blue", "Lower Third Blue", "song", "#800080",
                           { Box(50, 820, 1820, 220, "#1c41a8", 0, 5),
                             Text(75, 830, 1770, 200, "1", 80, "transparent", true, "left") }));
    out.push_back(Template("lower_third_color", "Lower Third Color", "song", "#800080",
                           { Box(50, 820, 1820, 220, "#9a0c72", 0, 5),
                             Text(75, 830, 1770, 200, "1", 80, "transparent", true, "left") }));
    out.push_back(Template("lower_third_pastel", "Lower Third Pastel", "song", "#800080",
                           { Box(50, 820, 1820, 220, "#c7d5ff", 0, 5),
                             Text(75, 830, 1770, 200, "1", 80, "transparent", true, "left", "Segoe UI", "", "#000000") }));

    // ---- presentation ----
    out.push_back(Template("header", "Header", "presentation", kTextAccent,
                           { Text(208.5, 428.5, 1500, 220, "Header", 180, "transparent", true, "center", "Segoe UI", "title") }));
    out.push_back(Template("text", "Text", "presentation", kTextAccent,
                           { Text(50.5, 35, 1820, 220, "Header", 120, "transparent", true, "left", "Segoe UI", "title"),
                             Text(50.5, 290, 1820, 750, "Text", 80, "transparent", false, "left", "Segoe UI", "text") }));
    out.push_back(Template("metadata", "Metadata", "presentation", kTextAccent,
                           { Text(30, 910, 1860, 150, "Metadata", 30, "transparent", false, "left", "Segoe UI", "notes", "#cccccc") }));
    out.push_back(Template("blue_header", "Panel Header", "presentation", "#747680",
                           { Box(-850, -600, 1600, 1600, "#141519"),
                             Text(720, 640, 1130, 210, "1", 120, "transparent", true, "left", "Arial", "title"),
                             Text(720, 850, 1130, 60, "2", 50, "transparent", false, "left", "Arial", "text") }));
    out.push_back(Template("blue_main", "Panel Content", "presentation", "#747680",
                           { Box(0, 0, 500, 1080, "#141519"),
                             Text(550, 50, 1320, 980, "1", 80, "transparent", true, "left", "Arial") }));
    out.push_back(Template("bullets", "Bullets", "presentation", "#747680",
                           { Text(50, 88, 1820, 904, "• Bullet 1\n• Bullet 2\n• Bullet 3", 100, "transparent", true, "left") }));

    // ---- scripture: verse boxes over the screen, reference below ----
    {
        std::vector<pres::ContentBlock> s = { Box(30, 30, 1860, 865, "#66000000", 20),
                                              Text(55, 45, 1810, 835, "{scripture_number} {scripture_text}", 80, "transparent", false, "left", "Segoe UI", "text"),
                                              Box(30, 900, 1860, 150, "transparent"),
                                              Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"),
                                              Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3") };
        out.push_back(Template("scripture", "Scripture", "scripture", kScriptureAccent, std::move(s)));
    }
    {
        std::vector<pres::ContentBlock> s = { Box(30, 40, 1860, 400, "#66000000", 20),
                                              Text(55, 50, 1810, 380, "{scripture1_number} {scripture1_text}", 70, "transparent", false, "left", "Segoe UI", "text"),
                                              Box(30, 475, 1860, 400, "#66000000", 20),
                                              Text(55, 485, 1810, 380, "{scripture2_number} {scripture2_text}", 70, "transparent", false, "left", "Segoe UI", "text"),
                                              Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"),
                                              Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3") };
        out.push_back(Template("scripture_2", "Scripture 2", "scripture", kScriptureAccent, std::move(s)));
    }
    {
        std::vector<pres::ContentBlock> s;
        for (int i = 0; i < 3; ++i) {
            s.push_back(Box(30, 40 + i * 280, 1860, 250, "#66000000", 20));
            s.push_back(Text(55, 50 + i * 280, 1810, 230, "{scripture" + std::to_string(i + 1) + "_number} {scripture" + std::to_string(i + 1) + "_text}", 60, "transparent", false, "left", "Segoe UI", "text"));
        }
        s.push_back(Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"));
        s.push_back(Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3"));
        out.push_back(Template("scripture_3", "Scripture 3", "scripture", kScriptureAccent, std::move(s)));
    }
    {
        std::vector<pres::ContentBlock> s;
        for (int i = 0; i < 4; ++i) {
            s.push_back(Box(30, 40 + i * 210, 1860, 200, "#66000000", 20));
            s.push_back(Text(55, 48 + i * 210, 1810, 184, "{scripture" + std::to_string(i + 1) + "_number} {scripture" + std::to_string(i + 1) + "_text}", 60, "transparent", false, "left", "Segoe UI", "text"));
        }
        s.push_back(Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"));
        s.push_back(Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3"));
        out.push_back(Template("scripture_4", "Scripture 4", "scripture", kScriptureAccent, std::move(s)));
    }

    // ---- scripture lower thirds: white bars with an orange reference chip ----
    {
        std::vector<pres::ContentBlock> s = { Box(30, 765, 1860, 238, "#ffffff", 20),
                                              Text(55, 775, 1810, 218, "{scripture_number} {scripture_text}", 80, "transparent", false, "left", "Segoe UI", "text", "#000000"),
                                              Box(1442, 960, 448, 88, "#ff851b", 10),
                                              Text(1445, 962, 442, 50, "{scripture_reference}", 40, "transparent", false, "left", "Segoe UI", "ref", "#000000"),
                                              Text(1445, 1005, 442, 40, "{scripture_name}", 30, "transparent", false, "left", "Segoe UI", "", "#000000") };
        out.push_back(Template("scripture_lt", "Scripture Lower Third", "scripture", kScriptureAccent, std::move(s)));
    }
    {
        std::vector<pres::ContentBlock> s = { Box(30, 765, 1860, 120, "#ffffff", 20),
                                              Text(55, 768, 1810, 114, "{scripture1_number} {scripture1_text}", 80, "transparent", false, "left", "Segoe UI", "text", "#000000"),
                                              Box(30, 885, 1860, 120, "#dddddd", 20),
                                              Text(55, 888, 1810, 114, "{scripture2_number} {scripture2_text}", 80, "transparent", false, "left", "Segoe UI", "text", "#000000"),
                                              Box(1442, 960, 448, 88, "#ff851b", 10),
                                              Text(1445, 962, 442, 50, "{scripture_reference}", 40, "transparent", false, "left", "Segoe UI", "ref", "#000000"),
                                              Text(1445, 1005, 442, 40, "{scripture_name}", 30, "transparent", false, "left", "Segoe UI", "", "#000000") };
        out.push_back(Template("scripture_lt_2", "Scripture Lower Third 2", "scripture", kScriptureAccent, std::move(s)));
    }

    // FreeShow's scripture formatting helpers: a bracketed or parenthesised aside in a dim italic.
    out.push_back(Template("brackets", "Brackets", "scripture", "#515151",
                           { Text(50, 88, 1820, 904, "[Brackets]", 60, "transparent", false, "center", "Segoe UI", "", "#999999", true) }));
    out.push_back(Template("parentheses", "Parentheses", "scripture", "#515151",
                           { Text(50, 88, 1820, 904, "(Parentheses)", 60, "transparent", false, "center", "Segoe UI", "", "#cccccc") }));

    // ---- table: the sermon layouts (The Table tab) — the same placeholder
    // machinery as scripture, its own content type so each tab lists only its
    // own layouts. Paragraphs stream into the "text" box exactly as verses do.
    {
        std::vector<pres::ContentBlock> s = { Box(30, 30, 1860, 865, "#66000000", 20),
                                              Text(55, 45, 1810, 835, "{scripture_number} {scripture_text}", 64, "transparent", false, "left", "Segoe UI", "text"),
                                              Box(30, 900, 1860, 150, "transparent"),
                                              Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"),
                                              Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3") };
        out.push_back(Template("table", "Table", "table", kTableAccent, std::move(s)));
    }
    {
        std::vector<pres::ContentBlock> s;
        for (int i = 0; i < 3; ++i) {
            s.push_back(Box(30, 40 + i * 280, 1860, 250, "#66000000", 20));
            s.push_back(Text(55, 50 + i * 280, 1810, 230, "{scripture" + std::to_string(i + 1) + "_number} {scripture" + std::to_string(i + 1) + "_text}", 54, "transparent", false, "left", "Segoe UI", "text"));
        }
        s.push_back(Text(30, 905, 1860, 80, "{scripture_reference}", 55, "transparent", false, "left", "Segoe UI", "ref", "#cccccc"));
        s.push_back(Text(30, 970, 1860, 70, "{scripture_name}", 40, "transparent", false, "left", "Segoe UI", "", "#b3b3b3"));
        out.push_back(Template("table_3", "Table 3", "table", kTableAccent, std::move(s)));
    }
    return out;
}

// A new template starts as a title and a body that show the slide's own fields.
std::vector<pres::ContentBlock> TemplateStarter() {
    return { Text(100, 90, 1720, 170, "Title", 90, "transparent", true, "center", "Segoe UI", "title"),
             Text(100, 300, 1720, 680, "Text", 70, "transparent", false, "center", "Segoe UI", "text") };
}

} // namespace

DesignLibraryConfig OverlayLibraryConfig() {
    DesignLibraryConfig c;
    c.noun = "overlay";
    c.defaultName = "Overlay";
    c.idPrefix = "ov";
    c.defaultCategories = { DesignCategory{ "visuals", "Visuals", "star", true } };
    c.defaultDesigns = ShippedOverlays();
    c.starterBlocks = OverlayStarter;
    // The two whole-screen treatments only overlays have (see DesignCatalogs.hpp).
    c.extraBlockKinds = { "vignette", "corners" };
    return c;
}

DesignLibraryConfig TemplateLibraryConfig() {
    DesignLibraryConfig c;
    c.noun = "template";
    c.defaultName = "Template";
    c.idPrefix = "tpl";
    // FreeShow's starter set: the song / presentation / scripture templates with their categories, and The Table's own sermon layouts.
    c.defaultCategories = { DesignCategory{ "song", "Song", "music", true },
                            DesignCategory{ "presentation", "Presentation", "presentation", true },
                            DesignCategory{ "scripture", "Scripture", "bookOpen", true },
                            DesignCategory{ "table", "The Table", "book", true } };
    c.defaultDesigns = ShippedTemplates();
    c.starterBlocks = TemplateStarter;
    return c;
}

std::unique_ptr<DesignLibrary> MakeOverlayLibrary(std::string storageFile) {
    return std::make_unique<DesignLibrary>(std::move(storageFile), OverlayLibraryConfig());
}

std::unique_ptr<DesignLibrary> MakeTemplateLibrary(std::string storageFile) {
    return std::make_unique<DesignLibrary>(std::move(storageFile), TemplateLibraryConfig());
}

} // namespace bps::library
