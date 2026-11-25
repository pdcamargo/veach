// The engine's own material dispatch: StandardMaterial only. Apps with materials of their own get a generated
// one instead (veach_add_path_tracer, docs/adr/0033-material-model.md).
import veach.material;

SurfaceSample evaluateSurface(uint kind, MaterialContext c) {
    return evaluate(StandardMaterial(), c);
}

/// Coverage only (alpha testing runs per candidate hit, so it must stay cheap).
float evaluateAlpha(uint kind, MaterialContext c) {
    return StandardMaterial().evalBaseColor(c).a;
}
