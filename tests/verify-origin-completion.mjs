// Bounded source-plumbing regression, not execution of real NGX/NR/AFW.
// Run with a workspace argument; legacy publication markers must fail this contract.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const root = path.resolve(process.argv[2] ?? '.');
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const source = read('OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp');
const helper = source.indexOf('static NVSDK_NGX_Result EvaluateFeatureImplementationDx12(');
const exported = source.indexOf('NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(');
assert(helper >= 0 && exported > helper, 'both SR routes need one common completion wrapper');
const implementation = source.slice(helper, exported);
const completion = source.slice(exported, source.indexOf('#pragma endregion', exported));
assert(implementation.includes('NVNGXProxy::D3D12_EvaluateFeature()('), 'native SR route missing');
assert(implementation.includes('TryEvaluateOptiFeature('), 'owned SR route missing');
assert(!implementation.includes('take_luke_origin_frame()'), 'native-only consumption bypasses common completion');
assert(!implementation.includes('mark_luke_color_origin('), 'native-only marker survived replacement');
assert.equal(source.match(/xrfg::mark_luke_color_origin\(/g)?.length, 1, 'exactly one completion writer');
const evaluate = completion.indexOf('const auto result = EvaluateFeatureImplementationDx12(');
const origin = completion.indexOf('const auto color_origin = ofxr_bridge::take_luke_origin_frame()');
const finalOutput = completion.indexOf('InParameters->Get(NVSDK_NGX_Parameter_Output, &final_output)');
const marker = completion.indexOf('xrfg::mark_luke_color_origin(native_list, final_output,');
assert(evaluate >= 0 && origin > evaluate && finalOutput > origin && marker > finalOutput,
    'stamp the restored final output after SR/postprocessing/NR, retaining its immutable guide');
assert(completion.includes('color_origin->color_origin_token, state,'),
    'the completion writer must receive its exact allocated 32-bit color token');
assert(!completion.includes('color_origin->publication, state,'),
    'publication must not be truncated or passed as the compact wire token');
assert(completion.includes('result == NVSDK_NGX_Result_Success && color_origin'), 'failed SR must not stamp');
assert(completion.includes('owned_sr ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS'), 'owned final DLSS state must be UAV');
assert(completion.includes('SameAdapter(final_device.Get(), color_origin->verified_producer_device.Get())'),
    'final resource must use the validated same-adapter ingress contract');
assert(!completion.includes('SameCanonicalIdentity(final_device.Get(),'),
    'resource-device aliases must not be rejected by a stricter canonical-device guard');
assert(completion.includes('SameCanonicalIdentity(list_device.Get(), color_origin->verified_producer_device.Get())'),
    'native command-list device identity must remain strict');
assert(completion.includes('Dx12Contexts.find(InFeatureHandle->Id)') &&
    completion.includes('context->second.feature->DisplayWidth()') &&
    completion.includes('context->second.feature->DisplayHeight()'),
    'owned final extent must come from authoritative creation-time display dimensions');
assert(!completion.includes('InParameters->Get(NVSDK_NGX_Parameter_OutWidth') &&
    !completion.includes('InParameters->Get(NVSDK_NGX_Parameter_OutHeight'),
    'owned Evaluate params need not repeat creation-only output dimensions');
for (const failure of ['Nested', 'MissingOutput', 'MissingContext', 'Dimensions', 'ResourceAdapter', 'ListIdentity', 'Unsupported'])
    assert(completion.includes(`kOrigin${failure}`), `missing marker validation condition ${failure}`);
assert(completion.includes('marker_result, 91, color_origin->publication'),
    'the single functional writer outcome must retain the full publication label');
for (const retired of ['luke_origin_observer', 'HookOriginObserver', 'ObserveNativeTrace',
    'luke_recording_token', 'bind_luke_sr_recording_token', 'ObserveOriginResourceWitness',
    'marker_failures, 92,', 'publication), 93,'])
    assert(!source.includes(retired), `retired observer route survived: ${retired}`);
const feature = read('OptiScaler/upscalers/dlss/DLSSFeature_Dx12.cpp');
assert(feature.indexOf('LukeRossDlssIngress::PublishD3D12Guide(') < feature.indexOf('nvResult = NVNGXProxy::D3D12_EvaluateFeature()('),
    'owned DLSS publication must precede native evaluation');
const pipeline = read('OptiScaler/upscalers/IFeature_Dx12.cpp');
assert(pipeline.indexOf('InParameters->Set(NVSDK_NGX_Parameter_Output, paramOutput)') >
    pipeline.indexOf('auto evalResult = EvaluateInternal('), 'owned pipeline must restore final output before completion');
const registry = read('third_party/ofxr/src/common/dlss_motion_vectors.cpp');
const originStart = registry.indexOf('std::shared_ptr<const DlssMotionVectorFrame> resolve_luke_guide_token(');
const originResolver = registry.slice(originStart, registry.indexOf('\nvoid report_dlss_motion_vector_status(', originStart));
assert(originStart >= 0 && !originResolver.includes('same_device(frame->output.Get(), consumer_queue)'),
    'unused original SR output cannot gate the actual-color AFW origin');
assert(originResolver.includes('std::uint32_t color_origin_token') &&
    originResolver.includes('frame->color_origin_token == color_origin_token'),
    'compact token must resolve an exact immutable retained guide');
assert(!registry.includes('resolve_luke_guide_origin('), 'superseded publication resolver survived');
assert(originResolver.includes('canonical_consumer_device') &&
    originResolver.includes('motion_snapshot_device') && originResolver.includes('depth_snapshot_device'),
    'actual snapshot allocation receipts must match the explicit native consumer device');
assert(originResolver.includes('canonical_consumer_device == nullptr') &&
    originResolver.includes('same_identity(frame->motion_snapshot_device.Get(),') &&
    originResolver.includes('same_identity(frame->depth_snapshot_device.Get(),'),
    'native consumer ownership is mandatory and must use canonical device identity');
assert(!originResolver.includes('same_device(frame->motion_vectors.Get(), consumer_queue)') &&
    !originResolver.includes('same_device(frame->depth.Get(), consumer_queue)'),
    'child GetDevice aliases must not replace actual snapshot allocation receipts');
assert(!originResolver.includes('GetAdapterLuid') && !originResolver.includes('same_adapter') &&
    !originResolver.includes('SameAdapter'), 'native ownership must not fall back to adapter matching');
assert(originResolver.includes('same_identity(frame->producer_queue.Get(), consumer_queue)') &&
    originResolver.includes('frame->producer_fence_value == 0') &&
    originResolver.includes('consumer_queue->Wait(frame->producer_fence.Get(),'),
    'real cross-queue submission receipt and wait cannot be replaced by same-device ownership');
assert(originResolver.includes('same_identity(frame->producer_fence_device.Get(),') &&
    !originResolver.includes('same_device(frame->producer_fence.Get(), consumer_queue)'),
    'cross-queue fence ownership must also use its actual creation-device receipt');
const snapshotStart = registry.indexOf('HRESULT snapshot_resource(');
const snapshotEnd = registry.indexOf('\nstruct Candidate', snapshotStart);
const snapshot = registry.slice(snapshotStart, snapshotEnd);
assert(snapshot.includes('allocation_receipt') && snapshot.includes('slot.allocation_device'),
    'snapshot ownership must come from the actual persistent allocation slot');
assert(snapshot.includes('same_identity') && snapshot.includes('allocation_device'),
    'snapshot slot reuse must preserve native allocation device identity');
assert(snapshot.includes('!same_identity(slot.allocation_device.Get(), allocation_device.Get())') &&
    snapshot.indexOf('slot.allocation_device = allocation_device') >
        snapshot.indexOf('result = allocation_device->CreateCommittedResource(') &&
    snapshot.includes('if (allocation_receipt) *allocation_receipt = slot.allocation_device'),
    'reuse cannot relabel a slot; receipt must follow actual successful native allocation');
const submissionStart = registry.indexOf('void report_dlss_motion_vector_submission(');
const submissionEnd = registry.indexOf('\nstd::shared_ptr<const DlssMotionVectorSet>', submissionStart);
const submission = registry.slice(submissionStart, submissionEnd);
assert(submission.includes('submission_device = frame->motion_snapshot_device') &&
    submission.includes('submission_device->CreateFence(') &&
    submission.includes('updated->producer_fence_device = stream.submission_fence_device') &&
    !submission.includes('queue->GetDevice('),
    'submission fence receipt must come from the actual snapshots, not a reported queue proxy device');
const layer = read('third_party/ofxr/src/layer/openxr_layer.cpp');
assert(layer.includes('origin_token, session->d3d12_queue.Get(), session->d3d12_device.Get()'),
    'exact lookup must receive the session native device explicitly');
const contextSource = read('OptiScaler/ofxr/ofxr_bridge_client.cpp');
const contextStart = contextSource.indexOf('int OFXR_GetLukeRossD3D12ContextV1(');
const context = contextSource.slice(contextStart, contextSource.indexOf('namespace ofxr_bridge', contextStart));
assert(context.includes('StreamlineHooks::getBorrowedNativeInterface(producer_device.Get(), &native_device)') &&
    context.indexOf('producer_device = std::move(canonical_device)') < context.indexOf('producer_device.CopyTo(device)'),
    'Luke native queue context must canonicalize its reported device before exporting ownership');
console.log('Origin completion and guide ownership source contracts: PASS');
