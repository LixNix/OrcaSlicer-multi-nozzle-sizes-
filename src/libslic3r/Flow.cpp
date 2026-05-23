#include "Flow.hpp"
#include "I18N.hpp"
#include "Print.hpp"
#include <cmath>
#include <assert.h>

#include <boost/algorithm/string/predicate.hpp>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

FlowErrorNegativeSpacing::FlowErrorNegativeSpacing() : 
	FlowError("Flow::spacing() produced negative spacing. Did you set some extrusion width too small?") {}

FlowErrorNegativeFlow::FlowErrorNegativeFlow() :
    FlowError("Flow::mm3_per_mm() produced negative flow. Did you set some extrusion width too small?") {}

// This static method returns a sane extrusion width default.
float Flow::auto_extrusion_width(FlowRole role, float nozzle_diameter)
{
    switch (role) {
    case frSupportMaterial:
    case frSupportMaterialInterface:
    case frSupportTransition:
    case frTopSolidInfill:
        return nozzle_diameter;
    default:
    case frExternalPerimeter:
    case frPerimeter:
    case frSolidInfill:
    case frBottomSurface:
    case frInfill:
        return 1.125f * nozzle_diameter;
    }
}

// Used by the Flow::extrusion_width() funtion to provide hints to the user on default extrusion width values,
// and to provide reasonable values to the PlaceholderParser.
static inline FlowRole opt_key_to_flow_role(const std::string &opt_key)
{
 	if (opt_key == "inner_wall_line_width" || 
 		// or all the defaults:
 		opt_key == "line_width" || opt_key == "initial_layer_line_width")
        return frPerimeter;
    else if (opt_key == "outer_wall_line_width")
        return frExternalPerimeter;
    else if (opt_key == "sparse_infill_line_width")
        return frInfill;
    else if (opt_key == "internal_solid_infill_line_width")
        return frSolidInfill;
	else if (opt_key == "top_surface_line_width")
		return frTopSolidInfill;
	else if (opt_key == "support_line_width")
    	return frSupportMaterial;
    else 
    	throw Slic3r::RuntimeError("opt_key_to_flow_role: invalid argument");
};

static inline void throw_on_missing_variable(const std::string &opt_key, const char *dependent_opt_key) 
{
	throw FlowErrorMissingVariable((boost::format(L("Failed to calculate line width of %1%. Cannot get value of \"%2%\" ")) % opt_key % dependent_opt_key).str());
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionFloatOrPercent* opt, const ConfigOptionResolver& config, const unsigned int first_printing_extruder)
{
	assert(opt != nullptr);

#if 0
// This is the logic used for skit / brim, but not for the rest of the 1st layer.
	if (opt->value == 0. && first_layer) {
		// The "initial_layer_line_width" was set to zero, try a substitute.
		opt = config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
		if (opt == nullptr)
    		throw_on_missing_variable(opt_key, "inner_wall_line_width");
	}
#endif

	if (opt->value == 0.) {
		// The role specific extrusion width value was set to zero, try the role non-specific extrusion width.
		opt = config.option<ConfigOptionFloatOrPercent>("line_width");
		if (opt == nullptr)
    		throw_on_missing_variable(opt_key, "line_width");
	}

    auto opt_nozzle_diameters = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters == nullptr)
        throw_on_missing_variable(opt_key, "nozzle_diameter");

    if (opt->percent) {
		return opt->get_abs_value(float(opt_nozzle_diameters->get_at(first_printing_extruder)));
	}

	if (opt->value == 0.) {
        // If user left option to 0, calculate a sane default width.
        return auto_extrusion_width(opt_key_to_flow_role(opt_key), float(opt_nozzle_diameters->get_at(first_printing_extruder)));
    }

	return opt->value;
}

// Used to provide hints to the user on default extrusion width values, and to provide reasonable values to the PlaceholderParser.
double Flow::extrusion_width(const std::string& opt_key, const ConfigOptionResolver &config, const unsigned int first_printing_extruder)
{
    return extrusion_width(opt_key, config.option<ConfigOptionFloatOrPercent>(opt_key), config, first_printing_extruder);
}

// This constructor builds a Flow object from an extrusion width config setting
// and other context properties.
Flow Flow::new_from_config_width(FlowRole role, const ConfigOptionFloatOrPercent &width, float nozzle_diameter, float height)
{
    if (height <= 0)
        throw Slic3r::InvalidArgument("Invalid flow height supplied to new_from_config_width()");

    float w;
    if (!width.percent  && width.value <= 0.) {
        // If user left option to 0, calculate a sane default width.
        w = auto_extrusion_width(role, nozzle_diameter);
    } else {
        // If user set a manual value, use it.
      w = float(width.get_abs_value(nozzle_diameter));
    }
    
    return Flow(w, height, rounded_rectangle_extrusion_spacing(w, height), nozzle_diameter, false);
}

ConfigOptionFloatOrPercent nozzle_aware_line_width(
    const PrintConfig &print_config, bool per_feature_filament,
    const ConfigOptionFloatOrPercent &width, unsigned int extruder_id)
{
    ConfigOptionFloatOrPercent result = width;

    // Per-extruder line width override (printer-level). When > 0 for this extruder, it
    // replaces the configured width (absolute mm or a percentage of the routed nozzle).
    bool overridden = false;
    if (extruder_id > 0 && extruder_id - 1 < print_config.extruder_line_width.values.size()) {
        const FloatOrPercent &override_v = print_config.extruder_line_width.get_at(extruder_id - 1);
        if (override_v.value > 0) {
            result = ConfigOptionFloatOrPercent(override_v.value, override_v.percent);
            overridden = true;
        }
    }

    // When per-feature filaments are enabled on a printer that mixes nozzle sizes, an absolute
    // mm width tuned for one nozzle is wrong on the others. Without an explicit override, fall
    // back to an auto width derived from the routed nozzle. Percentage widths already scale with
    // the nozzle, so leave those untouched. Uniform-nozzle printers keep their profile widths.
    if (!overridden && per_feature_filament && !result.percent && result.value > 0) {
        const std::vector<double> &nds = print_config.nozzle_diameter.values;
        bool mixed_nozzles = false;
        for (size_t i = 1; i < nds.size(); ++i)
            if (std::abs(nds[i] - nds.front()) > EPSILON) { mixed_nozzles = true; break; }
        if (mixed_nozzles)
            result = ConfigOptionFloatOrPercent(0., false); // 0 => Flow computes an auto width from the nozzle
    }

    return result;
}

size_t physical_extruder_for_filament(const PrintConfig &print_config, unsigned int filament_id)
{
    if (filament_id == 0)
        return 0;
    const size_t direct         = (size_t)(filament_id - 1);
    const size_t nozzle_count   = print_config.nozzle_diameter.values.size();
    const size_t filament_count = print_config.filament_diameter.values.size();
    // When the printer has at least as many nozzles as filaments, each filament has its own physical
    // extruder (a toolchanger, or any printer without AMS sharing a nozzle), so the filament index
    // already is the physical extruder. Only when more filaments share fewer nozzles (AMS) do we need
    // the filament -> extruder assignment from filament_map.
    if (filament_count <= nozzle_count)
        return direct;
    const std::vector<int> &fmap = print_config.filament_map.values;
    if (filament_id <= fmap.size() && fmap[filament_id - 1] >= 1)
        return (size_t)(fmap[filament_id - 1] - 1);
    // filament_map not populated for this filament yet: best effort.
    return direct < nozzle_count ? direct : 0;
}

// Adjust extrusion flow for new extrusion line spacing, maintaining the old spacing between extrusions.
Flow Flow::with_spacing(float new_spacing) const
{
    Flow out = *this;
    if (m_bridge) {
        // Diameter of the rounded extrusion.
        assert(m_width == m_height);
        float gap          = m_spacing - m_width;
        auto  new_diameter = new_spacing - gap;
        out.m_width        = out.m_height = new_diameter;
    } else {
        assert(m_width >= m_height);
        out.m_width += new_spacing - m_spacing;
        if (out.m_width < out.m_height)
            throw Slic3r::InvalidArgument(L("Invalid spacing supplied to Flow::with_spacing(), check your layer height and extrusion width"));
    }
    out.m_spacing = new_spacing;
    return out;
}

// Adjust the width / height of a rounded extrusion model to reach the prescribed cross section area while maintaining extrusion spacing.
Flow Flow::with_cross_section(float area_new) const
{
    assert(! m_bridge);
    assert(m_width >= m_height);

    // Adjust for bridge_flow, maintain the extrusion spacing.
    float area = this->mm3_per_mm();
    if (area_new > area + EPSILON) {
        // Increasing the flow rate.
        float new_full_spacing = area_new / m_height;
        if (new_full_spacing > m_spacing) {
            // Filling up the spacing without an air gap. Grow the extrusion in height.
            float height = area_new / m_spacing;
            return Flow(rounded_rectangle_extrusion_width_from_spacing(m_spacing, height), height, m_spacing, m_nozzle_diameter, false);
        } else {
            return this->with_width(rounded_rectangle_extrusion_width_from_spacing(area / m_height, m_height));
        }
    } else if (area_new < area - EPSILON) {
        // Decreasing the flow rate.
        float width_new = m_width - (area - area_new) / m_height;
        assert(width_new > 0);
        if (width_new > m_height) {
            // Shrink the extrusion width.
            return this->with_width(width_new);
        } else {
            // Create a rounded extrusion.
            auto dmr = float(sqrt(area_new / M_PI));
            return Flow(dmr, dmr, m_spacing, m_nozzle_diameter, false);
        }
    } else
        return *this;
}

float Flow::rounded_rectangle_extrusion_spacing(float width, float height)
{
    auto out = width - height * float(1. - 0.25 * PI);
    if (out <= 0.f)
        throw FlowErrorNegativeSpacing();
    return out;
}

float Flow::rounded_rectangle_extrusion_width_from_spacing(float spacing, float height)
{
    return float(spacing + height * (1. - 0.25 * PI));
}

float Flow::bridge_extrusion_spacing(float dmr)
{
    return dmr + BRIDGE_EXTRA_SPACING;
}

// This method returns extrusion volume per head move unit.
double Flow::mm3_per_mm() const
{
    float res = m_bridge ?
        // Area of a circle with dmr of this->width.
        float((m_width * m_width) * 0.25 * PI) :
        // Rectangle with semicircles at the ends. ~ h (w - 0.215 h)
        float(m_height * (m_width - m_height * (1. - 0.25 * PI)));
    //assert(res > 0.);
	if (res <= 0.)
		throw FlowErrorNegativeFlow();
    return res;
}

Flow support_material_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    const bool per_feature = object->print()->default_region_config().enable_per_feature_filament.value;
    const size_t physical_extruder = physical_extruder_for_filament(print_config, (unsigned int)object->config().support_filament.value);
    ConfigOptionFloatOrPercent width = (object->config().support_line_width.value > 0) ? object->config().support_line_width : object->config().line_width;
    width = nozzle_aware_line_width(print_config, per_feature, width, (unsigned int)(physical_extruder + 1));
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        // Nozzle diameter of the physical extruder the support filament is mapped to.
        float(print_config.nozzle_diameter.get_at(physical_extruder)),
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}
//BBS
Flow support_transition_flow(const PrintObject* object)
{
    //BBS: support transition of tree support is bridge flow
    const PrintConfig &print_config = object->print()->config();
    const size_t physical_extruder = physical_extruder_for_filament(print_config, (unsigned int)object->config().support_filament.value);
    float dmr = float(print_config.nozzle_diameter.get_at(physical_extruder));
    return Flow::bridging_flow(dmr, dmr);
}

Flow support_material_1st_layer_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    const bool per_feature = object->print()->default_region_config().enable_per_feature_filament.value;
    const size_t physical_extruder = physical_extruder_for_filament(print_config, (unsigned int)object->config().support_filament.value);
    const auto &width = (print_config.initial_layer_line_width.value > 0) ? print_config.initial_layer_line_width : object->config().support_line_width;
    ConfigOptionFloatOrPercent eff_width = (width.value > 0) ? width : object->config().line_width;
    eff_width = nozzle_aware_line_width(print_config, per_feature, eff_width, (unsigned int)(physical_extruder + 1));
    return Flow::new_from_config_width(
        frSupportMaterial,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        eff_width,
        float(print_config.nozzle_diameter.get_at(physical_extruder)),
        (layer_height > 0.f) ? layer_height : float(print_config.initial_layer_print_height.value));
}

Flow support_material_interface_flow(const PrintObject *object, float layer_height)
{
    const PrintConfig &print_config = object->print()->config();
    const bool per_feature = object->print()->default_region_config().enable_per_feature_filament.value;
    const size_t physical_extruder = physical_extruder_for_filament(print_config, (unsigned int)object->config().support_interface_filament.value);
    ConfigOptionFloatOrPercent width = (object->config().support_line_width > 0) ? object->config().support_line_width : object->config().line_width;
    width = nozzle_aware_line_width(print_config, per_feature, width, (unsigned int)(physical_extruder + 1));
    return Flow::new_from_config_width(
        frSupportMaterialInterface,
        // The width parameter accepted by new_from_config_width is of type ConfigOptionFloatOrPercent, the Flow class takes care of the percent to value substitution.
        width,
        // Nozzle diameter of the physical extruder the support interface filament is mapped to.
        float(print_config.nozzle_diameter.get_at(physical_extruder)),
        (layer_height > 0.f) ? layer_height : float(object->config().layer_height.value));
}

}
