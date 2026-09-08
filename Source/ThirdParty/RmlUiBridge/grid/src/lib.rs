use std::ffi::{c_char, c_void, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use taffy::prelude::*;
use taffy::style::{GridAutoTracks, GridTemplateTracks, Overflow};
use cssparser::{Parser, ParserInput, Token, ToCss};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Length { value: f32, kind: i32 }
#[repr(C)]
pub struct GridStyle {
    size: [Length; 2], min_size: [Length; 2], max_size: [Length; 2],
    margin: [Length; 4], padding: [Length; 4], border: [f32; 4],
    border_box: i32, overflow_x: i32, overflow_y: i32,
    columns: *const c_char, rows: *const c_char,
    auto_columns: *const c_char, auto_rows: *const c_char,
    areas: *const c_char, flow: *const c_char,
    column_start: *const c_char, column_end: *const c_char,
    row_start: *const c_char, row_end: *const c_char,
    justify_items: *const c_char, align_items: *const c_char,
    justify_self: *const c_char, align_self: *const c_char,
    justify_content: *const c_char, align_content: *const c_char,
    gap: [Length; 2],
    units: [f32; 5],
}
#[repr(C)]
pub struct GridRect { x: f32, y: f32, width: f32, height: f32, padding: [f32;4], margin: [f32;4] }
type Measure = unsafe extern "C" fn(*mut c_void, usize, f32, f32, f32, f32, *mut f32, *mut f32);
fn string<'a>(value: *const c_char) -> &'a str {
    if value.is_null() { "" } else { unsafe { CStr::from_ptr(value) }.to_str().unwrap_or("") }
}
fn dimension(v: Length) -> Dimension {
    match v.kind { 1 => Dimension::length(v.value), 2 => Dimension::percent(v.value / 100.0), _ => Dimension::auto() }
}
fn length(v: Length) -> LengthPercentage {
    if v.kind == 2 { LengthPercentage::percent(v.value / 100.0) } else { LengthPercentage::length(v.value) }
}
fn margin(v: Length) -> LengthPercentageAuto {
    match v.kind { 1 => LengthPercentageAuto::length(v.value), 2 => LengthPercentageAuto::percent(v.value / 100.0), _ => LengthPercentageAuto::auto() }
}
fn rect<T: Copy>(v: [T; 4]) -> taffy::geometry::Rect<T> {
    taffy::geometry::Rect { top: v[0], right: v[1], bottom: v[2], left: v[3] }
}
fn parse<T: std::str::FromStr>(s: &str) -> Result<T, String> where T::Err: std::fmt::Display {
    s.parse().map_err(|e| format!("Invalid grid value '{s}': {e}"))
}
fn resolve_units(s: &str, units: &[f32; 5]) -> Result<String, String> {
    fn tokens<'i>(p: &mut Parser<'i, '_>, units: &[f32; 5]) -> Result<String, cssparser::ParseError<'i, ()>> {
        let mut out = String::new();
        while !p.is_exhausted() {
            let token = p.next_including_whitespace()?.clone();
            if let Token::Dimension { value, ref unit, .. } = token {
                let scale = match unit.to_ascii_lowercase().as_str() {
                    "em" => Some(units[0]), "rem" => Some(units[1]), "dp" => Some(units[2]),
                    "vw" => Some(units[3]), "vh" => Some(units[4]),
                    "in" => Some(96.0), "cm" => Some(96.0/2.54), "mm" => Some(96.0/25.4),
                    "pt" => Some(96.0/72.0), "pc" => Some(16.0), _ => None
                };
                if let Some(scale) = scale { out.push_str(&format!("{}px", value * scale)); continue; }
            }
            token.to_css(&mut out).unwrap();
            let closing = match token { Token::Function(_) | Token::ParenthesisBlock => Some(')'), Token::SquareBracketBlock => Some(']'), _ => None };
            if let Some(closing) = closing { out.push_str(&p.parse_nested_block(|p| tokens(p, units))?); out.push(closing); }
        }
        Ok(out)
    }
    tokens(&mut Parser::new(&mut ParserInput::new(s)), units).map_err(|_| format!("Invalid track tokens: {s}"))
}
fn tracks(s: &str) -> Result<GridTemplateTracks<String, GridTemplateComponent<String>>, String> {
    if s.is_empty() || s == "none" { return Ok(GridTemplateTracks::default()); }
    let result = parse::<GridTemplateTracks<String, GridTemplateComponent<String>>>(s)?;
    let mut total = 0;
    let mut automatic = 0;
    for track in &result.tracks {
        match track {
            GridTemplateComponent::Single(_) => total += 1,
            GridTemplateComponent::Repeat(r) => match r.count {
                RepetitionCount::Count(n) => { if n == 0 { return Err("repeat count must be positive".into()); } total += n as usize * r.tracks.len(); },
                _ => { automatic += 1; total += r.tracks.len(); }
            }
        }
    }
    if total > 4096 || automatic > 1 { return Err("Grid exceeds 4096 explicit tracks or contains multiple automatic repetitions".into()); }
    Ok(result)
}
fn placement(s: &str) -> Result<GridPlacement<String>, String> {
    if s == "0" || s == "span 0" { return Err("Grid line/span cannot be zero".into()); }
    parse(if s.is_empty() { "auto" } else { s })
}
fn areas(s: &str) -> Result<Option<taffy::style::GridTemplateAreas<String>>, String> {
    if s.is_empty() || s == "none" { return Ok(None); }
    let mut input = cssparser::ParserInput::new(s);
    let mut parser = cssparser::Parser::new(&mut input);
    let mut rows = Vec::new();
    while !parser.is_exhausted() {
        let row = parser.expect_string().map_err(|_| "grid-template-areas requires quoted rows")?;
        rows.push(row.split_whitespace().map(str::to_owned).collect::<Vec<_>>());
    }
    if rows.is_empty() || rows[0].is_empty() || rows.iter().any(|r| r.len() != rows[0].len()) { return Err("Grid area rows must have equal nonzero lengths".into()); }
    if rows.len() > 4096 || rows[0].len() > 4096 { return Err("Too many grid area tracks".into()); }
    let mut bounds = std::collections::BTreeMap::<String, (usize, usize, usize, usize)>::new();
    for (y, row) in rows.iter().enumerate() { for (x, name) in row.iter().enumerate() {
        if name.chars().all(|c| c == '.') { continue; }
        let b = bounds.entry(name.clone()).or_insert((x, y, x, y)); b.2 = b.2.max(x); b.3 = b.3.max(y);
    }}
    let mut result = Vec::new();
    for (name, (x0, y0, x1, y1)) in bounds {
        for row in &rows[y0..=y1] { if row[x0..=x1].iter().any(|cell| cell != &name) { return Err(format!("Grid area '{name}' is not rectangular")); } }
        result.push(taffy::style::GridTemplateArea { name: name.into(), row_start: (y0 + 1) as u16, row_end: (y1 + 2) as u16, column_start: (x0 + 1) as u16, column_end: (x1 + 2) as u16 });
    }
    Ok(Some(taffy::style::GridTemplateAreas { areas: result, row_count: rows.len() as u16, column_count: rows[0].len() as u16 }))
}
fn alignment(s: &str) -> &str {
    match s { "" | "normal" => "stretch", "flex-start" => "start", "flex-end" => "end", other => other }
}
fn make_style(raw: &GridStyle, container: bool) -> Result<Style, String> {
    let mut style = Style { display: Display::Grid, box_sizing: if raw.border_box != 0 { BoxSizing::BorderBox } else { BoxSizing::ContentBox }, ..Style::default() };
    style.size = Size { width: dimension(raw.size[0]), height: dimension(raw.size[1]) };
    style.min_size = Size { width: margin(raw.min_size[0]), height: margin(raw.min_size[1]) };
    style.max_size = Size { width: margin(raw.max_size[0]), height: margin(raw.max_size[1]) };
    style.margin = rect(raw.margin.map(margin)); style.padding = rect(raw.padding.map(length));
    style.border = rect(raw.border.map(LengthPercentage::length));
    style.overflow = taffy::geometry::Point { x: if raw.overflow_x != 0 { Overflow::Hidden } else { Overflow::Visible }, y: if raw.overflow_y != 0 { Overflow::Hidden } else { Overflow::Visible } };
    if container {
        let columns = tracks(&resolve_units(string(raw.columns), &raw.units)?)?; let rows = tracks(&resolve_units(string(raw.rows), &raw.units)?)?;
        style.grid_template_columns = columns.tracks; style.grid_template_column_names = columns.line_names;
        style.grid_template_rows = rows.tracks; style.grid_template_row_names = rows.line_names;
        style.grid_auto_columns = parse::<GridAutoTracks>(&resolve_units(string(raw.auto_columns), &raw.units)?)?.0;
        style.grid_auto_rows = parse::<GridAutoTracks>(&resolve_units(string(raw.auto_rows), &raw.units)?)?.0;
        style.grid_template_areas = areas(string(raw.areas))?;
        style.grid_auto_flow = parse(string(raw.flow))?;
        style.justify_items = Some(parse(alignment(string(raw.justify_items)))?);
        style.align_items = Some(parse(alignment(string(raw.align_items)))?);
        style.justify_content = Some(parse(alignment(string(raw.justify_content)))?);
        style.align_content = Some(parse(alignment(string(raw.align_content)))?);
        style.gap = Size { width: length(raw.gap[0]), height: length(raw.gap[1]) };
    } else {
        style.grid_column = Line { start: placement(string(raw.column_start))?, end: placement(string(raw.column_end))? };
        style.grid_row = Line { start: placement(string(raw.row_start))?, end: placement(string(raw.row_end))? };
        if !matches!(string(raw.justify_self), "" | "auto") { style.justify_self = Some(parse(alignment(string(raw.justify_self)))?); }
        if !matches!(string(raw.align_self), "" | "auto") { style.align_self = Some(parse(alignment(string(raw.align_self)))?); }
    }
    Ok(style)
}
#[no_mangle]
pub extern "C" fn RmlGrid_Validate(property: *const c_char, value: *const c_char) -> i32 {
    catch_unwind(|| {
        let s = string(value);
        let valid = match string(property) {
            "tracks" => resolve_units(s, &[1.0;5]).is_ok_and(|s| tracks(&s).is_ok()),
            "auto-tracks" => resolve_units(s, &[1.0;5]).is_ok_and(|s| parse::<GridAutoTracks>(&s).is_ok()),
            "custom-ident" => { let mut input=ParserInput::new(s); let mut p=Parser::new(&mut input); p.expect_ident().is_ok_and(|s| !matches!(s.as_ref(), "auto" | "span")) && p.is_exhausted() },
            "placement" => placement(s).is_ok(),
            "areas" => areas(s).is_ok(),
            "flow" => parse::<GridAutoFlow>(s).is_ok(),
            "alignment" => s == "auto" || parse::<AlignItems>(alignment(s)).is_ok(),
            _ => false,
        };
        i32::from(valid)
    }).unwrap_or(0)
}
fn available(v: f32) -> AvailableSpace { if v >= 0.0 { AvailableSpace::Definite(v) } else if v == -1.0 { AvailableSpace::MinContent } else { AvailableSpace::MaxContent } }
fn encode(v: AvailableSpace) -> f32 { match v { AvailableSpace::Definite(v) => v, AvailableSpace::MinContent => -1.0, AvailableSpace::MaxContent => -2.0 } }
#[no_mangle]
pub unsafe extern "C" fn RmlGrid_Layout(raw: *const GridStyle, items: *const GridStyle, count: usize,
    measure: Measure, user: *mut c_void, results: *mut GridRect, width: *mut f32, height: *mut f32, error: *mut c_char, capacity: usize) -> i32 {
    let outcome = catch_unwind(AssertUnwindSafe(|| -> Result<(), String> {
        if raw.is_null() || width.is_null() || height.is_null() || count > 16384 || (count > 0 && (items.is_null() || results.is_null())) { return Err("Invalid grid layout arguments".into()); }
        let raw = &*raw;
        let mut tree: TaffyTree<usize> = TaffyTree::new(); tree.disable_rounding();
        let mut nodes = Vec::with_capacity(count);
        for i in 0..count { nodes.push(tree.new_leaf_with_context(make_style(&*items.add(i), false)?, i).map_err(|e| e.to_string())?); }
        let root = tree.new_with_children(make_style(raw, true)?, &nodes).map_err(|e| e.to_string())?;
        tree.compute_layout_with_measure(root, Size { width: available(raw.size[0].value), height: if raw.size[1].kind == 0 { AvailableSpace::MaxContent } else { available(raw.size[1].value) } },
            |inputs, _, context, style| {
                taffy::compute_leaf_layout(inputs, style, |_, _| 0.0, |known, space| {
                    let mut width = 0.0; let mut height = 0.0;
                    if let Some(index) = context { measure(user, *index, known.width.unwrap_or(-1.0), known.height.unwrap_or(-1.0), encode(space.width), encode(space.height), &mut width, &mut height); }
                    Size { width: width.max(0.0), height: height.max(0.0) }
                })
            }).map_err(|e| e.to_string())?;
        for (i, node) in nodes.iter().enumerate() { let l = tree.layout(*node).map_err(|e| e.to_string())?; *results.add(i) = GridRect { x: l.location.x, y: l.location.y, width: l.size.width, height: l.size.height,
            padding: [l.padding.top,l.padding.right,l.padding.bottom,l.padding.left], margin: [l.margin.top,l.margin.right,l.margin.bottom,l.margin.left] }; }
        let root_size = tree.layout(root).map_err(|e| e.to_string())?.size;
        *width = root_size.width; *height = root_size.height;
        Ok(())
    }));
    let message = match outcome { Ok(Ok(())) => return 1, Ok(Err(e)) => e, Err(_) => "Grid layout panic was contained at the ABI boundary".into() };
    if !error.is_null() && capacity > 0 { let n = message.len().min(capacity - 1); std::ptr::copy_nonoverlapping(message.as_ptr(), error as *mut u8, n); *error.add(n) = 0; }
    0
}
