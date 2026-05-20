#include "Exception.hpp"
#include "Print.hpp"

namespace Slic3r {

// 1-based extruder identifier for this region and role.
unsigned int PrintRegion::extruder(FlowRole role) const
{
    // The per-feature filament feature is opt-in: when disabled, outer_wall_filament,
    // top_surface_filament and bottom_surface_filament are ignored and every feature uses
    // wall_filament / solid_infill_filament / sparse_infill_filament as in stock OrcaSlicer.
    const bool per_feature = m_config.enable_per_feature_filament.value;
    size_t extruder = 0;
    if (role == frExternalPerimeter) {
        extruder = (per_feature && m_config.outer_wall_filament.value > 0) ? m_config.outer_wall_filament : m_config.wall_filament;
    }
    else if (role == frPerimeter)
        extruder = m_config.wall_filament;
    else if (role == frInfill)
        extruder = m_config.sparse_infill_filament;
    else if (role == frTopSolidInfill) {
        extruder = (per_feature && m_config.top_surface_filament.value > 0) ? m_config.top_surface_filament : m_config.solid_infill_filament;
    }
    else if (role == frBottomSurface) {
        extruder = (per_feature && m_config.bottom_surface_filament.value > 0) ? m_config.bottom_surface_filament : m_config.solid_infill_filament;
    }
    else if (role == frSolidInfill)
        extruder = m_config.solid_infill_filament;
    else
        throw Slic3r::InvalidArgument("Unknown role");
    return extruder;
}

Flow PrintRegion::flow(const PrintObject &object, FlowRole role, double layer_height, bool first_layer) const
{
    const PrintConfig          &print_config = object.print()->config();
    ConfigOptionFloatOrPercent config_width;
    // Get extrusion width from configuration.
    // (might be an absolute value, or a percent value, or zero for auto)
    if (first_layer && print_config.initial_layer_line_width.value > 0) {
        config_width = print_config.initial_layer_line_width;
    } else if (role == frExternalPerimeter) {
        config_width = m_config.outer_wall_line_width;
    } else if (role == frPerimeter) {
        config_width = m_config.inner_wall_line_width;
    } else if (role == frInfill) {
        config_width = m_config.sparse_infill_line_width;
    } else if (role == frSolidInfill || role == frBottomSurface) {
        // Bottom surfaces share the internal solid infill line-width setting; the extruder
        // routing (and thus the nozzle diameter used below) is what differs between them.
        config_width = m_config.internal_solid_infill_line_width;
    } else if (role == frTopSolidInfill) {
        config_width = m_config.top_surface_line_width;
    } else {
        throw Slic3r::InvalidArgument("Unknown role");
    }

    if (config_width.value == 0)
        config_width = object.config().line_width;

    // Resolve which 1-based extruder will print this feature.
    unsigned int extruder_id = this->extruder(role);
    // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
    auto nozzle_diameter = float(print_config.nozzle_diameter.get_at(extruder_id - 1));

    // Per-extruder line width override (printer-level). When > 0 for this extruder,
    // it replaces the per-feature line width. Supports both absolute mm and a
    // percentage of the routed extruder's nozzle, so "100%" makes each toolhead use
    // its own nozzle's width across all features (the common case for toolchangers
    // with mixed nozzle sizes).
    bool overridden = false;
    if (extruder_id > 0 && extruder_id - 1 < print_config.extruder_line_width.values.size()) {
        const FloatOrPercent &override_v = print_config.extruder_line_width.get_at(extruder_id - 1);
        if (override_v.value > 0) {
            config_width = ConfigOptionFloatOrPercent(override_v.value, override_v.percent);
            overridden = true;
        }
    }

    // When per-feature filaments are enabled on a printer that mixes nozzle sizes, a feature
    // can be routed to a toolhead whose nozzle differs from the one the print profile's
    // absolute line widths were tuned for. A fixed mm width (e.g. 0.42) is wrong on a 0.2 or
    // 0.8 mm nozzle, so without an explicit per-extruder override fall back to an auto width
    // derived from the routed nozzle. Widths expressed as a percentage already scale with the
    // nozzle, so those are left untouched and give the user a way to keep a custom ratio across
    // all toolheads. Printers whose nozzles are all the same size keep their profile widths.
    if (!overridden && m_config.enable_per_feature_filament.value && !config_width.percent && config_width.value > 0) {
        const std::vector<double> &nds = print_config.nozzle_diameter.values;
        bool mixed_nozzles = false;
        for (size_t i = 1; i < nds.size(); ++i)
            if (std::abs(nds[i] - nds.front()) > EPSILON) { mixed_nozzles = true; break; }
        if (mixed_nozzles)
            config_width = ConfigOptionFloatOrPercent(0., false); // 0 => Flow computes an auto width from the nozzle
    }

    return Flow::new_from_config_width(role, config_width, nozzle_diameter, float(layer_height));
}

coordf_t PrintRegion::nozzle_dmr_avg(const PrintConfig &print_config) const
{
    return (print_config.nozzle_diameter.get_at(m_config.wall_filament.value    - 1) + 
            print_config.nozzle_diameter.get_at(m_config.sparse_infill_filament.value       - 1) + 
            print_config.nozzle_diameter.get_at(m_config.solid_infill_filament.value - 1)) / 3.;
}

coordf_t PrintRegion::bridging_height_avg(const PrintConfig &print_config) const
{
    return this->nozzle_dmr_avg(print_config) * sqrt(m_config.bridge_flow.value);
}

void PrintRegion::collect_object_printing_extruders(const PrintConfig &print_config, const PrintRegionConfig &region_config, const bool has_brim, std::vector<unsigned int> &object_extruders)
{
    // These checks reflect the same logic used in the GUI for enabling/disabling extruder selection fields.
    // BBS
    auto num_extruders = (int)print_config.filament_diameter.size();
    auto emplace_extruder = [num_extruders, &object_extruders](int extruder_id) {
    	int i = std::max(0, extruder_id - 1);
        object_extruders.emplace_back((i >= num_extruders) ? 0 : i);
    };
    const bool per_feature = region_config.enable_per_feature_filament.value;
    if (region_config.wall_loops.value > 0 || has_brim)
    	emplace_extruder(region_config.wall_filament);
    if (per_feature && region_config.wall_loops.value > 0 && region_config.outer_wall_filament.value > 0)
        emplace_extruder(region_config.outer_wall_filament);
    if (region_config.sparse_infill_density.value > 0)
    	emplace_extruder(region_config.sparse_infill_filament);
    if (region_config.top_shell_layers.value > 0 || region_config.bottom_shell_layers.value > 0)
    	emplace_extruder(region_config.solid_infill_filament);
    if (per_feature && region_config.top_shell_layers.value > 0 && region_config.top_surface_filament.value > 0)
    	emplace_extruder(region_config.top_surface_filament);
    if (per_feature && region_config.bottom_shell_layers.value > 0 && region_config.bottom_surface_filament.value > 0)
    	emplace_extruder(region_config.bottom_surface_filament);
}

void PrintRegion::collect_object_printing_extruders(const Print &print, std::vector<unsigned int> &object_extruders) const
{
    // PrintRegion, if used by some PrintObject, shall have all the extruders set to an existing printer extruder.
    // If not, then there must be something wrong with the Print::apply() function.
#ifndef NDEBUG
    // BBS
    auto num_extruders = int(print.config().filament_diameter.size());
    assert(this->config().wall_filament    <= num_extruders);
    assert(this->config().sparse_infill_filament       <= num_extruders);
    assert(this->config().solid_infill_filament <= num_extruders);
#endif
    collect_object_printing_extruders(print.config(), this->config(), print.has_brim(), object_extruders);
}

}
