# Krita Features Proposal for Drawpile

## Executive Summary

This document analyzes Krita's feature set and proposes high-value features that could be implemented in Drawpile to enhance its drawing, animation, and collaboration capabilities. Drawpile already incorporates several Krita-inspired features (stabilizer, tablet support, blend modes, color schemes, ORA compatibility), but significant gaps remain.

---

## Current Drawpile Features (Krita-Inspired)

| Feature | Source | Status |
|---------|--------|--------|
| Brush Stabilizer | Krita | ✅ Implemented |
| Tablet-friendly Sliders | Krita | ✅ Implemented |
| Windows Ink / Wintab / X11 Tablet Support | Krita | ✅ Implemented |
| "Greater Density" Blend Mode | Krita's "Greater" | ✅ Implemented |
| "Marker" Blend Mode | Krita's "Alpha Darken" | ✅ Implemented |
| Inherit Alpha / Preserve Alpha | Krita | ✅ Implemented |
| Color Circle with Gamut Masks | Krita's Artistic Color Selector | ✅ Implemented |
| ORA Compatibility (alpha preserve, layer modes) | Krita 5.2+ | ✅ Implemented |
| Selection Outline Generator | Krita's KisOutlineGenerator | ✅ Implemented |
| Zoom Levels | Krita-inspired | ✅ Implemented |
| Krita-esque Themes (Bright/Dark/Darker) | Krita | ✅ Implemented |
| Onion Skin Icons | Krita | ✅ Implemented |

---

## High-Priority Feature Proposals

### 1. Advanced Brush Engines
**Priority: HIGH** | **Effort: HIGH**

Krita has 9+ brush engines; Drawpile currently has Classic + MyPaint.

| Engine | Description | Use Case |
|--------|-------------|----------|
| **Color Smudge** | Mixes colors on canvas like wet paint | Painterly blending, traditional media simulation |
| **Filter Brush** | Applies filters (blur, sharpen, desaturate, hue shift) as brush strokes | Photo editing, non-destructive local adjustments |
| **Deform Brush** | Liquefy-style transformations via brush | Quick warping, caricature, corrections |
| **Shape Engine** | Draws vector shapes (stars, polygons, custom) as brush strokes | Patterns, textures, decorative elements |
| **Particle Engine** | Sprays particles (sparks, leaves, snow) | Effects, textures, creative brushes |
| **Sketch Engine** | Hatching/cross-hatching simulation | Technical illustration, comics |
| **Grid Engine** | Pattern-based brushes | Textures, halftones, comics |
| **Clone Engine** | Clones from another layer/position | Healing, pattern replication |

**Implementation Path**: Extend `drawdance::BrushEngine` plugin architecture. Start with Color Smudge and Filter Brush as highest impact.

---

### 2. Filter Layers & Filter Masks (Non-Destructive Editing)
**Priority: HIGH** | **Effort: MEDIUM-HIGH**

Krita's filter layers/masks allow applying filters (blur, levels, color adjustments) non-destructively in the layer stack.

| Feature | Description |
|---------|-------------|
| **Filter Layer** | Applies filter to all layers below in group |
| **Filter Mask** | Applies filter to single layer, driven by grayscale mask |
| **Supported Filters** | Blur, Sharpen, Levels, Curves, Color Balance, HSV, Desaturate, Threshold, etc. |
| **Mask Integration** | Transparency masks control where filter applies |

**Drawpile Integration**: Add `DP_LAYER_TYPE_FILTER` and `DP_LAYER_TYPE_FILTER_MASK` to layer types. Extend `drawdance::LayerProps` with filter chain. Leverage existing tile cache for real-time preview.

---

### 3. Transform Masks
**Priority: MEDIUM-HIGH** | **Effort: MEDIUM**

Non-destructive transform (scale, rotate, perspective, warp, cage) as layer stack items.

| Feature | Description |
|---------|-------------|
| **Transform Mask** | Affine/perspective/cage warp on layer/group |
| **Non-Destructive** | Original pixels unchanged; transform computed at render |
| **Animateable** | Transform parameters keyframeable in timeline |
| **Clone/File Layer Support** | Works on referenced content |

**Drawpile Integration**: Add `DP_LAYER_TYPE_TRANSFORM_MASK`. Extend render pipeline to apply transform matrices per-tile.

---

### 4. Vector Layers & Vector Tools
**Priority: MEDIUM** | **Effort: HIGH**

Full vector graphics support (currently Drawpile is raster-only).

| Tool | Description |
|------|-------------|
| **Vector Layer** | Contains SVG-compatible vector objects |
| **Selection Tools on Vector** | All paint tools (except freehand) create vector shapes |
| **Vector Editing** | Node tool, path operations (union, difference, etc.) |
| **Text on Vector** | Text as editable vector objects |
| **Export** | SVG, PDF export |

**Note**: Major architectural change. Consider as long-term goal.

---

### 5. Advanced Selection Tools & Operations
**Priority: HIGH** | **Effort: MEDIUM**

Drawpile has basic selection; Krita has comprehensive toolset.

| Tool/Feature | Status | Notes |
|--------------|--------|-------|
| Rectangular/Elliptical | ✅ | Basic |
| Freehand/Lasso | ✅ | Basic |
| Polygonal | ❌ | Click-to-add-points |
| **Similar Color (Magic Wand)** | ❌ | **High value** - select by color similarity |
| **Contiguous Selection** | ❌ | Flood fill selection with fuzziness |
| **Path Selection** | ❌ | Vector path-based selection |
| **Magnetic Selection** | ❌ | Edge-snapping lasso |
| **Selection Modifiers** | Partial | Add/Subtract/Intersect (Alt/Shift) |
| **Selection Operations** | ❌ | Grow, Shrink, Feather, Border, Scale, Transform |
| **Global Selection Mask** | ❌ | Treat selection as editable grayscale layer |

**Implementation**: Extend `SelectionModel` and `drawdance::Selection` with new selection types. Add selection operation menu.

---

### 6. Enhanced Animation Timeline
**Priority: HIGH** | **Effort: MEDIUM**

Drawpile has basic timeline; Krita's is production-ready.

| Feature | Drawpile | Krita | Gap |
|---------|----------|-------|-----|
| Transport Controls (Play/Pause/Step) | Basic | Full | Add speed, loop, drop frames |
| Onion Skin Docker | Basic | Advanced | Opacity, frame range, color labels |
| **Auto-Key Mode** | ❌ | ✅ | Auto-create keyframes on edit |
| **Frame Rate Control** | ❌ | ✅ | FPS setting, playback range |
| **Color Labels for Keyframes** | ❌ | ✅ | Visual organization |
| **Clone Frames vs Duplicates** | ❌ | ✅ | Linked vs independent copies |
| **Pin Layers to Timeline** | ❌ | ✅ | Show inactive anim layers |
| **Audio Scrubbing** | ❌ | ✅ | Timeline audio preview |
| **Export Video/GIF** | ❌ | ✅ | FFmpeg integration |

**Implementation**: Extend `TimelineModel`, `TimelineItemModel`. Add `drawdance::Timeline` features. Integrate FFmpeg for export.

---

### 7. Layer Stack Enhancements
**Priority: MEDIUM-HIGH** | **Effort: LOW-MEDIUM**

| Feature | Description |
|---------|-------------|
| **Color Labels** | Right-click layer → assign color (7 colors) for organization |
| **Layer Filtering** | Filter by name, color label, type (search box in layer docker) |
| **Alpha Inheritance** | "Inherit Alpha" (clipping) per-layer, not just group-level |
| **Checkbox Selection Mode** | Tablet-friendly multi-select without Ctrl/Shift |
| **Layer Styles/Effects** | Drop shadow, outer glow, bevel, stroke (non-destructive) |
| **Quick Layer Actions** | Ctrl+E merge down, Ctrl+Shift+E merge all, duplicate shortcuts |

---

### 8. Brush System Enhancements
**Priority: MEDIUM** | **Effort: LOW-MEDIUM**

| Feature | Description |
|---------|-------------|
| **Brush Tagging System** | Tags for organization (blending, texture, ink, favorites) |
| **Sensor Curve Corners** | Ctrl+click curve point → sharp corner (linear segments) |
| **Advanced Sensor Inputs** | Tilt, rotation, barrel rotation, azimuth, altitude |
| **Workflow Buttons Docker** | Customizable buttons: select tool, brush, color, run script |
| **Brush Preset Search/Filter** | Filter by name, tag, engine |

---

### 9. Wrap-Around Mode (Seamless Patterns)
**Priority: MEDIUM** | **Effort: LOW**

| Feature | Description |
|---------|-------------|
| **Toggle** | View → Wrap Around Mode |
| **Behavior** | Canvas tiles infinitely in X/Y; painting on one tile updates all |
| **Use Case** | Texture creation, pattern design, game assets |
| **Export** | Single tile export with seamless guarantee |

**Implementation**: Modify `CanvasModel` render to tile. Add viewport offset tracking.

---

### 10. Assistant Tools (Drawing Guides)
**Priority: MEDIUM** | **Effort: MEDIUM**

| Assistant | Description |
|-----------|-------------|
| **Perspective Grid** | 1/2/3-point perspective with vanishing points |
| **Ellipse Assistant** | Perfect ellipses/circles at any angle |
| **Ruler** | Straight line guide |
| **Spline/Ruler** | Curved guide |
| **Concentric Ellipse** | Multiple aligned ellipses |
| **Fish Eye** | Spherical perspective |
| **Snap to Assistant** | Brush strokes snap to active guide |

**Implementation**: New `Assistant` system in `drawdance`, integrate with brush engine stroke processing.

---

### 11. Color Management & Proofing
**Priority: LOW-MEDIUM** | **Effort: MEDIUM**

| Feature | Description |
|---------|-------------|
| **Soft Proofing** | Simulate output color space (sRGB, CMYK profiles) |
| **Gamut Masks** | Limit color picker to specific gamut (already partial) |
| **OCIO Support** | OpenColorIO for film/VFX workflows |
| **Color Space Conversion** | Per-layer color space tags |

---

### 12. Python Scripting / Plugin API
**Priority: LOW** | **Effort: HIGH**

| Feature | Description |
|---------|-------------|
| **Python Plugin API** | Extend Drawpile via Python (like Krita) |
| **Script Docker** | Run scripts, bind to shortcuts |
| **Automation** | Batch export, layer processing, custom tools |
| **Workflow Buttons** | Python-driven custom UI buttons |

---

## Implementation Roadmap

### Phase 1: Quick Wins (1-2 months)
1. **Similar Color / Magic Wand Selection** - High user value, moderate effort
2. **Selection Operations** (Grow, Shrink, Feather) - Build on existing selection
3. **Layer Color Labels** - UI only, minimal backend
4. **Layer Filter by Name** - UI filter on layer list
5. **Brush Tagging** - Data model + UI
6. **Wrap-Around Mode** - Render modification only

### Phase 2: Core Enhancements (3-6 months)
1. **Filter Brush Engine** - Leverage existing filter infrastructure
2. **Color Smudge Engine** - New engine type
3. **Enhanced Timeline** (Auto-key, FPS, Onion Skin improvements)
4. **Transform Masks** - Non-destructive transform layer type
5. **Assistant Tools** (Ruler, Perspective Grid) - New system

### Phase 3: Advanced Features (6-12 months)
1. **Filter Layers & Filter Masks** - Full non-destructive filter stack
2. **Vector Layers** (if architectural decision made)
3. **Python Scripting API** - Plugin infrastructure
4. **Full Animation Export** (Video/GIF via FFmpeg)
5. **Deform Brush Engine** - Liquefy-as-brush

---

## Technical Considerations

### Network Synchronization
- New layer types (filter, transform mask, vector) need protocol extensions
- Brush engine strokes must be deterministic for replay
- Consider bandwidth for complex brush engines

### Performance
- Filter layers: Tile-based processing, GPU acceleration where possible
- Transform masks: Matrix composition per-tile
- Wrap-around: Efficient viewport tiling

### Compatibility
- ORA export/import must handle new layer types
- Krita compatibility for filter/transform masks
- MyPaint brush compatibility maintained

---

## Conclusion

**Top 5 Recommended Features** for immediate impact:

1. **Magic Wand / Similar Color Selection** - Fundamental missing tool
2. **Filter Brush Engine** - Unique creative capability, leverages existing filters
3. **Enhanced Timeline (Auto-key, Onion Skin)** - Critical for animation users
4. **Layer Color Labels + Filtering** - Workflow improvement, low effort
5. **Wrap-Around Mode** - Niche but powerful for texture artists

These align with Drawpile's collaborative focus while closing the feature gap with Krita for serious digital painting.