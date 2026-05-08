#ifndef AFIS_DIAGRAM_H
#define AFIS_DIAGRAM_H

#include <string>
#include <vector>

#include "cfg.h"
#include "dependency.h"
#include "ir.h"

namespace afis {

struct WorkflowStepDiagram {
    std::string title;
    std::string detail;
};

bool WriteCfgDot(const CFG& cfg, const std::string& path, std::string& error);
std::string BuildCfgSvgMarkup(const CFG& cfg);
bool WriteCfgSvg(const CFG& cfg, const std::string& path, std::string& error);
bool WriteDependencyDot(const Program& program,
                        const DependencyGraph& graph,
                        const std::string& path,
                        std::string& error);
std::string BuildDependencySvgMarkup(const Program& program, const DependencyGraph& graph);
bool WriteDependencySvg(const Program& program,
                        const DependencyGraph& graph,
                        const std::string& path,
                        std::string& error);
bool WriteWorkflowDot(const std::vector<WorkflowStepDiagram>& steps,
                      const std::string& path,
                      std::string& error);
std::string BuildWorkflowSvgMarkup(const std::vector<WorkflowStepDiagram>& steps);
bool WriteWorkflowSvg(const std::vector<WorkflowStepDiagram>& steps,
                      const std::string& path,
                      std::string& error);

}  // namespace afis

#endif
