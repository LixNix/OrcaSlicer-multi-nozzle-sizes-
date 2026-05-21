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

    // Resolve which 1-based filament will print this feature, then map it to the physical
    // extruder/nozzle that filament is assigned to (filament_map). On multi-nozzle printers
    // (e.g. H2D/X2D) with more filaments than nozzles, nozzle_diameter and the per-extruder
    // overrides are indexed by physical extruder, not by filament, so this mapping is required
    // for correct per-feature line widths.
    unsigned int filament_id       = this->extruder(role);
    size_t       physical_extruder = physical_extruder_for_filament(print_config, filament_id);
    auto nozzle_diameter = float(print_config.nozzle_diameter.get_at(physical_extruder));

    // Apply the per-extruder line-width override and the mixed-nozzle auto-width fallback.
    // See nozzle_aware_line_width() in Flow.cpp; supports use the same helper. The override is
    // indexed by physical extruder, so pass the 1-based physical extruder id.
    config_width = nozzle_aware_line_width(print_config, m_config.enable_per_feature_filament.value, config_width, (unsigned int)(physical_extruder + 1));

    return Flow::new_from_config_width(role, config_width, nozzle_diameter, float(layer_height));
}

coordf_t PrintRegion::nozzle_dmr_avg(const PrintConfig &print_config) const
{
    // Map each filament to its physical extruder before reading the nozzle diameter, so the
    // average is correct on multi-nozzle printers with more filaments than nozzles.
    return (print_config.nozzle_diameter.get_at(physical_extruder_for_filament(print_config, m_config.wall_filament.value)) +
            print_config.nozzle_diameter.get_at(physical_extruder_for_filament(print_config, m_config.sparse_infill_filament.value)) +
            print_config.nozzle_diameter.get_at(physical_extruder_for_filament(print_config, m_config.solid_infill_filament.value))) / 3.;
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
