"""Request BGRA client targets only for the verified simpleDRM path."""
from pathlib import Path
root=Path('/home/a6l/android/a6l-lineage24/external/drm_hwcomposer/hwc3')
archive=Path(__file__).resolve().parents[1]/'research/android-surface-v44-20260917/format-negotiation'
archive.mkdir(exist_ok=False)
def patch(name,changes):
    p=root/name;s=p.read_text();(archive/(name+'.before')).write_text(s)
    for old,new in changes:
        assert s.count(old)==1,(name,old)
        s=s.replace(old,new)
    p.write_text(s);(archive/name).write_text(s)
patch('CommandResultWriter.h',[
 ('  std::optional<DisplayRequest> display_request_changes;',
  '  std::optional<DisplayRequest> display_request_changes;\n  std::optional<ClientTargetPropertyWithBrightness> client_target_property;'),
 ('           display_request_changes.has_value();','           display_request_changes.has_value() || client_target_property.has_value();'),
 ('    display_request_changes.reset();','    display_request_changes.reset();\n    client_target_property.reset();'),
 ('    if (changes.display_request_changes) {\n      results_->emplace_back(*changes.display_request_changes);\n    }',
  '    if (changes.display_request_changes) {\n      results_->emplace_back(*changes.display_request_changes);\n    }\n    if (changes.client_target_property) {\n      results_->emplace_back(*changes.client_target_property);\n    }')])
patch('ComposerClient.cpp',[
 ('    cmd_result_writer_->AddChanges(changes);',
 '''    // SurfaceFlinger otherwise keeps its default RGBA client target, which
    // simpleDRM cannot scan out. Negotiate BGRA through the standard HWC result;
    // the existing client-target-only ARGB->XRGB view preserves its color bytes.
    if (!display->IsInHeadlessMode() &&
        display->GetPipe().device->GetName() == "simpledrm") {
      ClientTargetPropertyWithBrightness target{};
      target.display = display_handle;
      target.clientTargetProperty.pixelFormat = common::PixelFormat::BGRA_8888;
      target.clientTargetProperty.dataspace = common::Dataspace::SRGB;
      target.brightness = 1.0F;
      target.dimmingStage = DimmingStage::LINEAR;
      changes.client_target_property = target;
    }
    cmd_result_writer_->AddChanges(changes);''')])
print(archive)
