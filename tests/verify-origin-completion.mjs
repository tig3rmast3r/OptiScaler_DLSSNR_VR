// Bounded source-plumbing regression, not execution of real NGX/NR/AFW.
// Run with a workspace argument; legacy publication markers must fail this contract.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
const root = path.resolve(process.argv[2] ?? '.');
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const source = read('OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp');
const helper = source.indexOf('static NVSDK_NGX_Result EvaluateFeatureImplementationDx12(');
const writer = source.indexOf('HRESULT LukeRossDlssIngress::CompleteD3D12Color(');
const exported = source.indexOf('NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(');
assert(helper >= 0 && writer > helper && exported > writer, 'shared completion writer missing');
const implementation = source.slice(helper, writer);
const completion = source.slice(writer, exported);
const wrapper = source.slice(exported, source.indexOf('#pragma endregion', exported));
assert(implementation.includes('NVNGXProxy::D3D12_EvaluateFeature()('), 'native SR route missing');
assert(implementation.includes('TryEvaluateOptiFeature('), 'owned SR route missing');
assert(!implementation.includes('take_luke_origin_frame()') && !implementation.includes('mark_luke_color_origin('),
    'SR implementation must not consume/write final identity');
assert.equal(source.match(/xrfg::mark_luke_color_origin\(/g)?.length, 1, 'one functional writer, not an API-specific duplicate');
assert(wrapper.indexOf('const auto result = EvaluateFeatureImplementationDx12(') <
       wrapper.indexOf('LukeRossDlssIngress::CompleteD3D12Color('), 'D3D12 must complete only after all SR/postprocessing');
assert(wrapper.includes('result == NVSDK_NGX_Result_Success') &&
    wrapper.includes('context->second.feature.get()'), 'failed SR cannot stamp and final extent needs owned metadata');
const origin = completion.indexOf('const auto color_origin = ofxr_bridge::take_luke_origin_frame()');
const finalOutput = completion.indexOf('parameters->Get(NVSDK_NGX_Parameter_Output, &final_output)');
const marker = completion.indexOf('xrfg::mark_luke_color_origin(native_list, final_output,');
assert(origin >= 0 && finalOutput > origin && marker > finalOutput, 'writer must take exact retained origin and restored output');
assert(completion.includes('color_origin->color_origin_token, state,') &&
    !completion.includes('color_origin->publication, state,'), 'never truncate publication into compact token');
assert(completion.includes('owned_sr ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS'), 'owned final color state must be UAV');
assert(completion.includes('SameAdapter(final_device.Get(), color_origin->verified_producer_device.Get())') &&
    completion.includes('d3d_owner::same(list_device.Get(), color_origin->verified_producer_device.Get())'),
    'resource adapter compatibility and exact list backing owner remain distinct requirements');
assert(completion.includes('feature->DisplayWidth()') && completion.includes('feature->DisplayHeight()'),
    'authoritative final display extent required for either API');
for (const failure of ['Nested', 'MissingOutput', 'MissingContext', 'Dimensions', 'ResourceAdapter', 'ListIdentity', 'Unsupported'])
    assert(completion.includes(`kOrigin${failure}`), `missing marker validation condition ${failure}`);
assert(completion.includes('marker_result, 91, color_origin->publication'), 'keep full writer outcome identity');
const vk = read('OptiScaler/upscalers/IFeature_Vk.cpp');
assert(vk.indexOf('paramOutput->Resource.ImageViewInfo.Image = originalOutput.Image') <
    vk.indexOf('_lukeCapture->Evaluate(InCmdBuffer, InParameters)'), 'capture follows restored native Vulkan pipeline');
for (const native of ['dlss/DLSSFeature_Vk.cpp', 'dlssd/DLSSDFeature_Vk.cpp']) {
    const feature = read('OptiScaler/upscalers/' + native);
    assert(feature.includes('NVNGXProxy::VULKAN_EvaluateFeature()(') &&
        !feature.includes('NVNGXProxy::D3D12_EvaluateFeature()('), 'do not replace native Vulkan SR/RR');
}
assert(vk.includes('GetUpscalerType() == Upscaler::DLSS || GetUpscalerType() == Upscaler::DLSSD') &&
    vk.includes('parameters, _native, InteropQueue()') &&
    vk.includes('CompleteD3D12Color(list, parameters, _native, true)'), 'SR/RR share explicit queue and one completion writer');
const interop = read('OptiScaler/upscalers/IFeature_VkwDx12.cpp');
assert(interop.includes('CapturesCompletedOutput() ? OutCopy2.get() : OutCopy.get()') &&
    interop.includes('CaptureExternalOwnership(InCmdList, true)') &&
    interop.includes('CaptureExternalOwnership(b.VulkanCopyCommandBuffer[frame], false)') &&
    interop.includes('wait.pValues = &CopyBackValues[frame]') &&
    interop.includes('parameters->Set(keys[i], values[i])'), 'separate descriptor rings, GPU ownership, slot completion and parameter restoration required');
const producerBroker = read('OptiScaler/hooks/LukeVulkanProducer.h');
assert(interop.includes('ofxr_bridge::request_luke_vulkan_context(') &&
    producerBroker.includes('factory->EnumAdapterByLuid(found->second.luid,') &&
    producerBroker.includes('if (devices.size() != 1) return E_INVALIDARG;'),
    'Vulkan producer determines exact D3D12 adapter; no title or primary GPU guess');
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
const nativeResolver = read('third_party/ofxr/include/xrfg/d3d12_native_binding.hpp');
const nativeEntry = read('OptiScaler/hooks/D3D_NativeInterface.h');
assert(nativeEntry.includes('d3d12_native_binding.hpp') &&
    nativeEntry.includes('namespace d3d_native = xrfg::d3d_native;') &&
    !nativeEntry.includes('inline bool get_borrowed'),
    'producer must use the single shared native-interface implementation');
assert(nativeResolver.includes('streamline_native_guid') &&
    nativeResolver.includes('reshade_unwrapped_guid') && nativeResolver.includes('next->Release()') &&
    nativeResolver.includes('visited[8]') && nativeResolver.includes('FAILED(result) || !next'),
    'Luke wrapper resolution must balance both native QI contracts and reject malformed or cyclic chains');
assert(source.includes('d3d_native::get_borrowed(object, &native, receipt)') &&
    !source.includes('ResolveStreamlineNative'),
    'all Luke ingress/completion native resolution must use the replacement resolver');
assert(context.includes('d3d_native::get_borrowed(producer_device.Get(), &native_device)') &&
    context.indexOf('producer_device = std::move(canonical_device)') < context.indexOf('producer_device.CopyTo(device)'),
    'Luke native queue context must canonicalize its reported device before exporting ownership');
const ingressStart = source.indexOf('static void PublishOfxrDlssGuides(');
const ingress = source.slice(ingressStart, source.indexOf('void LukeRossDlssIngress::PublishD3D12Guide', ingressStart));
assert(ingress.includes('xrfg::luke_ingress_device_failures({') &&
    ingress.includes('d3d_owner::same(queueDevice.Get(), commandDevice.Get(), &ownerReceipt)') &&
    ingress.includes('queueListOwner});'),
    'ingress must require canonical identity or a complete bidirectional backing proof');
assert(ingress.includes('publication.verified_native_device = queueDevice.Get()') &&
    ingress.includes('publication, queueDevice.Get()') &&
    ingress.includes('ensure_d3d12_submission_hook(queueDevice.Get())'),
    'allocation, submission hook and context export must agree on the queue device owner');
const owner = read('OptiScaler/hooks/D3D_DeviceOwner.h');
assert(owner.includes('result.verified = result.mask == 63') &&
    owner.includes('received == sent') && owner.includes('std::array<Entry, 8>') &&
    owner.includes('result.steps[6] = left->SetPrivateData(receipt_guid, 0, nullptr)') &&
    owner.includes('entry.left = a; entry.right = b'),
    'device owner proof must require both exact round trips/cleanup, be bounded and pin identities');
assert(ingress.includes('packets.fetch_add(1, std::memory_order_relaxed) < 8') &&
    ingress.includes('pack(objectQueries[i], receipts[i].get_device)') &&
    ingress.includes('pack(receipts[i].native_query, receipts[i].retain)'),
    'bounded packet must contain existing object/device HRESULT receipts');
assert(ingress.indexOf('if (deviceFailures)') < ingress.indexOf('publication.verified_native_device'),
    'diagnostic receipts must not bypass failed ownership admission');
console.log('Origin completion and guide ownership source contracts: PASS');
const afw = read('third_party/ofxr/src/d3d12/luke_afw.cpp');
assert(afw.includes('luke_motion_srv_format(motion_desc.Format) == DXGI_FORMAT_UNKNOWN') &&
    afw.includes('srv.Format=index==0?luke_motion_srv_format(field->GetDesc().Format):DXGI_FORMAT_R32_FLOAT') &&
    afw.includes('clean.Format = kLukeAfwMotionFormat') &&
    afw.includes('motion_description.Format == kLukeAfwMotionFormat') &&
    afw.includes('texture.Format = kLukeAfwMotionFormat') &&
    !afw.includes('description.Format != DXGI_FORMAT_R16G16_FLOAT'),
    'float2 inputs must use their actual typed SRV and one normalized RG16F AFW output');
const motionFormat = read('third_party/ofxr/include/xrfg/luke_motion_format.hpp');
assert(motionFormat.includes('case DXGI_FORMAT_R16G16B16A16_FLOAT:') &&
    motionFormat.includes('case DXGI_FORMAT_R32G32B32A32_FLOAT: return format;') &&
    motionFormat.includes('kLukeAfwMotionFormat = DXGI_FORMAT_R16G16_FLOAT'),
    'typed float4 XY inputs must preserve actual SRV format and normalized RG16F output');
console.log('Luke motion input/output format contract: PASS');
const composition = read('third_party/ofxr/src/d3d12/luke_compose_motion.hlsl');
const bridge = read('OptiScaler/ofxr/ofxr_bridge_client.cpp');
const motionStore = read('third_party/ofxr/src/common/dlss_motion_vectors.cpp');
assert(composition.includes('CurrentDepth.Load') && composition.includes('same.xy/same.w-opposite.xy/opposite.w') &&
    afw.includes('luke_native_motion_constants(guide,') &&
    motionStore.includes('frame->previous_same_native_camera=dlss_native_camera_history(*stream.previous)') &&
    !composition.includes('PreviousMotion') && !afw.includes('prior_motion') &&
    afw.includes('params.motion_vectors_type = Normal;'),
    'depth-projected native camera rebase physically replaces adjacent-field sampling; Normal for every mode');
assert(bridge.includes('embedded.raw_clip_to_previous_clip = productionCameraToken ? std::array<float,16>{} : g_active_streamline_camera.clip_to_previous_clip') &&
    bridge.includes('embedded.raw_camera_motion_included = productionCameraToken ? -1 : g_active_streamline_camera.camera_motion_included') &&
    motionStore.includes('frame->raw_clip_to_previous_clip = publication.raw_clip_to_previous_clip'),
    'declared Streamline metadata remains immutable; native producer must not borrow unreported Streamline motion history');
console.log('Native depth/camera motion rebase contract: PASS');
const sourceTrace=read('OptiScaler/hooks/LukeSourceTrace.h');
const sourceContext=read('OptiScaler/hooks/LukeSourceContext.h');
const sourceHooks=read('OptiScaler/hooks/Streamline_Hooks.cpp');
const nativePipeline=read('OptiScaler/upscalers/IFeature_Vk.cpp');
assert(afw.includes('params.motion_scale[0] = guide.scale_x *') &&
    afw.includes('params.motion_scale[1] = guide.scale_y *') &&
    !afw.includes('params.motion_scale[0] = -guide.scale_x *'),
    'rejected sign control must be absent: source trace uses V30071 geometry');
assert(sourceTrace.includes('p->conversionFormula') && sourceTrace.includes('t.extent.left') &&
    sourceTrace.includes('baseArrayLayer') && sourceTrace.includes('NVSDK_NGX_Parameter_MV_Scale_X') &&
    !sourceTrace.includes('ReadProcessMemory') && !sourceTrace.includes('CopyTexture') &&
    !sourceTrace.includes('.Set('),
    'source receipts inspect borrowed identity/precision/scalars only, never alter maps or GPU work');
assert(sourceHooks.includes('const bool attachResourceHooks = o_slSetTag || o_slSetTagForFrame;') &&
    sourceHooks.includes('trace(true); return result;') &&
    sourceHooks.includes('luke_source::Scope traceScope(') &&
    sourceContext.includes('~Scope() { active = previous; }') &&
    read('OptiScaler/upscalers/dlss/DLSSFeature_Vk.cpp').includes('luke_source::native(*InParameters,0,') &&
    read('OptiScaler/upscalers/dlss/DLSSFeature_Vk.cpp').includes('luke_source::native(*InParameters,1,') &&
    read('OptiScaler/upscalers/dlssd/DLSSDFeature_Vk.cpp').includes('luke_source::native(*InParameters,0,') &&
    read('OptiScaler/upscalers/dlssd/DLSSDFeature_Vk.cpp').includes('luke_source::native(*InParameters,1,') &&
    nativePipeline.includes('luke_source::native(*InParameters,2,'),
    'same-evaluation source trace covers tag forwarding/nested scope/native SR/native final postpass');
console.log('Luke same-evaluation source contract trace: PASS');
const ramFilter=read('third_party/ofxr/src/common/ram_timing.cpp');
const ramTest=read('third_party/ofxr/tests/ram_timing_tests.cpp');
assert(ramFilter.includes('selected_luke_source_stage(a)') &&
    ramFilter.includes('{320ULL, 368ULL, 416ULL}') &&
    ramFilter.includes('{480ULL, 520ULL, 560ULL}') &&
    ramTest.includes('source contract receipt lost before F11 save'),
    'source receipts must survive the active pose-profile recorder and F11 saver');
console.log('Luke source trace RAM/F11 retention contract: PASS');
const vkHooks = read('OptiScaler/hooks/Vulkan_Hooks.cpp');
const vkCopy = read('OptiScaler/shaders/resource_copy/RC_Vk.cpp');
assert(vkHooks.includes('captureFeatures.prepare(localCreteInfo,') &&
    vkHooks.includes('vulkan_capture::record(*pDevice, extendedFormats)') &&
    vk.includes('vulkan_capture::enabled(InDevice)') &&
    vkCopy.includes('vulkan_copy::typed_shader(rc_spv, format)') &&
    interop.includes('InParam->Resource.ImageViewInfo.Format))'),
    'typed Vulkan copies require the exact destination format and actually enabled device feature');
assert(interop.includes('if (!CapturesCompletedOutput() && !AutoExposure())') &&
    interop.includes('if (!CapturesCompletedOutput() && !Config::Instance()->DisableReactiveMask.value_or(false))'),
    'completed-color capture cannot change native SR exposure/reactive configuration');
console.log('Native Vulkan capture feature/format/no-SR-mutation contracts: PASS');
const librarySearch = read('OptiScaler/Util.cpp');
assert(librarySearch.includes('library_search::accept_streamline_candidate(fileName.wstring(), isDlssgOutput)') &&
    !librarySearch.includes('if (isDlssgOutput || !IsSubpath(normalizedPath, normalizedStreamlinePath))'),
    'replace FG-dependent exclusion: legitimate SR/RR files must remain discoverable');
console.log('Native SR/RR library discovery contract: PASS');
const negotiationOwnerSource = read('third_party/ofxr/src/layer/openxr_layer.cpp');
const negotiation = negotiationOwnerSource.slice(negotiationOwnerSource.lastIndexOf('extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface('));
assert(negotiation.indexOf('xrfg::embedded::set_layer_active(true)') >
    negotiation.indexOf('layer_request->createApiLayerInstance = layer_create_api_layer_instance'),
    'embedded ownership must be established by validated negotiation, not only an external caller');
const bridgeClient = read('OptiScaler/ofxr/ofxr_bridge_client.cpp');
assert(bridgeClient.includes('if (xrfg::embedded::layer_active()) return {};') &&
    !bridgeClient.includes('g_embedded_layer_active'),
    'layer and publisher must share a single ownership state');
console.log('Embedded negotiation/publication ownership contract: PASS');
assert(interop.includes('NVNGX_Parameters captureParameters(API::DX12, false)') &&
    interop.includes('luke_vulkan::copy_capture_metadata(*InParameters, captureParameters)') &&
    interop.includes('EvaluateInteropFeature(cmdList, &captureParameters)') &&
    !bridgeClient.includes('register_luke_vulkan_queue'),
    'capture uses a local typed D3D12 map and early device-lifetime owner');
console.log('Vulkan local parameter map / early producer ownership contract: PASS');
