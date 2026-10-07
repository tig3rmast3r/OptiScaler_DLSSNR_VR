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
    vk.indexOf('!_lukeCapture->EvaluateCompleted('),
    'completed-color/AER guide capture follows restored native Vulkan pipeline');
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
assert(composition.includes('CurrentDepth.Load') && composition.includes('same.xy/same.w-opposite.xy/opposite.w-') &&
    composition.includes('stereo.xy/stereo.w-stereo_far.xy/stereo_far.w') &&
    afw.includes('luke_native_motion_constants(guide,') &&
    afw.includes('const bool native_camera_contract = guide.camera_frame_token_valid;') &&
    afw.includes('if(native_camera_contract && !have_native_motion)') &&
    afw.includes('if (!temporal_camera_valid) return HRESULT_FROM_WIN32(ERROR_NOT_READY);') &&
    afw.includes('window_to_guide_transform(') &&
    motionStore.includes('frame->previous_same_native_camera=dlss_native_camera_history(*stream.previous)') &&
    !composition.includes('PreviousMotion') && !afw.includes('prior_motion') &&
    afw.includes('params.motion_vectors_type = Normal;'),
    'depth-projected native camera rebase physically replaces adjacent-field sampling; Normal for every mode');
assert(bridge.includes('embedded.raw_clip_to_previous_clip = productionCameraToken ? std::array<float,16>{} : g_active_streamline_camera.clip_to_previous_clip') &&
    bridge.includes('embedded.raw_camera_motion_included = productionCameraToken ? -1 : g_active_streamline_camera.camera_motion_included') &&
    motionStore.includes('frame->raw_clip_to_previous_clip = publication.raw_clip_to_previous_clip'),
    'declared Streamline metadata remains immutable diagnostic evidence, not a motion correction');
console.log('Unified depth-aware native/producer camera correction; Normal in every mode: PASS');
const sourceHooks=read('OptiScaler/hooks/Streamline_Hooks.cpp');
const nativePipeline=read('OptiScaler/upscalers/IFeature_Vk.cpp');
assert(afw.includes('params.motion_scale[0] = guide.scale_x *') &&
    afw.includes('params.motion_scale[1] = guide.scale_y *') &&
    !afw.includes('params.motion_scale[0] = -guide.scale_x *'),
    'rejected sign control must stay absent');
for (const text of [sourceHooks, bridge, nativePipeline, interop,
    read('OptiScaler/upscalers/dlss/DLSSFeature_Vk.cpp'),
    read('OptiScaler/upscalers/dlssd/DLSSDFeature_Vk.cpp')]) {
    assert(!text.includes('luke_source::') && !text.includes('LukeSourceTrace') &&
        !text.includes('LukeSourceContext') && !text.includes('LogNativeMotionHistory'),
        'release ingress must not retain temporary source tracing/native history probes');
}
assert(sourceHooks.includes('ofxr_bridge::cache_streamline_camera(') &&
    sourceHooks.includes('return o_slSetConstants(values,frame,viewport);') &&
    bridge.includes('if (candidate.valid && candidate.frame == frame)'),
    'removing diagnostics must preserve exact production camera cache and forwarding');
console.log('Luke release ingress / temporary trace removal: PASS');
const ramFilter=read('third_party/ofxr/src/common/ram_timing.cpp');
const ramTest=read('third_party/ofxr/tests/ram_timing_tests.cpp');
assert(!ramFilter.includes('selected_luke_source_stage') &&
    ramTest.includes('pose profile filtered compact AFW or cadence telemetry'),
    'compact F11 telemetry must survive after extended source stages are retired');
console.log('Compact RAM/F11 retention contract: PASS');
assert(nativePipeline.includes('_lukeAERMotion.reset();') &&
    nativePipeline.includes('EvaluateCompleted(') &&
    nativePipeline.includes('InteropQueue());') &&
    nativePipeline.includes('_captureMotion.valid?&_captureMotion.resource:nullptr') &&
    read('OptiScaler/upscalers/IFeature_VkwDx12.cpp').includes('ResolveCaptureMotion(InParameters, &paramMv)') &&
    read('OptiScaler/upscalers/dlss/DLSSFeature_Vk.cpp').includes('PrepareLukeMotion(InCmdBuffer, InParameters)') &&
    read('OptiScaler/upscalers/dlssd/DLSSDFeature_Vk.cpp').includes('PrepareLukeMotion(InCmdBuffer, InParameters)') &&
    afw.includes('params.motion_vectors_type = Normal;') &&
    !bridge.includes('motionFromOtherEye') &&
    !motionStore.includes('motion_from_other_eye'),
    'native Vulkan original AER descriptor feeds custom correction, never double-corrects with FromOtherEye');
console.log('Native Vulkan original AER motion contract: PASS');
for (const native of ['dlss/DLSSFeature_Vk.cpp', 'dlssd/DLSSDFeature_Vk.cpp']) {
    const text=read('OptiScaler/upscalers/'+native);
    assert(text.indexOf('PrepareLukeMotion(InCmdBuffer, InParameters)') <
        text.indexOf('NVNGXProxy::VULKAN_EvaluateFeature()(InCmdBuffer,'),
        'native SR/RR must freeze GPU motion before evaluating NGX');
}
const earlyCopy=interop.slice(interop.indexOf('bool IFeature_VkwDx12::PrepareCaptureMotion('),
    interop.indexOf('bool IFeature_VkwDx12::ProcessVulkanTextures('));
assert(earlyCopy.includes('WaitCaptureSlot(frame)') && earlyCopy.includes('WaitCaptureSlot(slot)') &&
    earlyCopy.includes('CopyTextureFromVkToDx12(command, original, &vkMv') &&
    earlyCopy.indexOf('CopyTextureFromVkToDx12') < earlyCopy.indexOf('FrozenMotion.publish'),
    'existing GPU motion pass must freeze after slot/recreation guards, before publishing receipt');
assert(interop.includes('FrozenMotion.matches(InCmdList, _frameCount)') &&
    interop.includes('if (!CapturesCompletedOutput() && paramMv != nullptr)') &&
    nativePipeline.includes('ClearCaptureMotion();'),
    'completed native capture must consume exact early pixels, no late-copy fallback');
console.log('Native Vulkan pre-NGX GPU motion freeze: PASS');
const captures=read('third_party/ofxr/src/common/two_frame_capture.cpp');
assert(!afw.includes('moving_input_capture') && !captures.includes('automatic_pending') &&
    !captures.includes('automatic_requested') && !captures.includes('request_moving_inputs') &&
    captures.includes('if (!state.session && pressed)') &&
    captures.includes('motion_current_to_middle_same') && captures.includes('motion_current_to_previous_same') &&
    afw.includes('contract.motion_previous_publication = impl_->previous_source_publication[source_eye]'),
    'manual F10 must retain exact inputs/matrices without any automatic capture');
console.log('Manual-only F10 input capture: PASS');
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
assert(interop.includes('luke_vulkan::create_capture_queue(InteropDevice.Get(), xrQueue.Get(), &Dx12CommandQueue)') &&
    !interop.includes('InteropDevice.ReleaseAndGetAddressOf(), &Dx12CommandQueue'),
    'Native Vulkan capture must share the early owner device, never its XR consumer queue');
console.log('Native Vulkan capture / XR queue isolation: PASS');
const producerCamera = read('third_party/ofxr/include/xrfg/luke_pose_camera.hpp');
const gameCameraConfig = read('third_party/ofxr/include/xrfg/luke_world_scale_config.hpp');
assert(gameCameraConfig.includes('_wcsicmp(section, L"DOOMTDA") == 0') &&
    gameCameraConfig.includes('LukeProducerCameraAxes::right_down_forward') &&
    !gameCameraConfig.includes('forward_sign') &&
    (afw.match(/luke_game_camera_config_from_ini\(/g) || []).length === 1 &&
    (afw.match(/impl_->camera_origin,/g) || []).length === 3 &&
    (afw.match(/impl_->camera_axes,/g) || []).length === 3 &&
    afw.includes('const auto source_position = origin.position_relative(guide.camera_position);') &&
    afw.includes('impl_->camera_origin.update(') &&
    afw.includes('impl_->frame_buffers_ready = false;') &&
    !afw.includes('kLukeDiagnosticForwardSign') &&
    !afw.includes('measure_camera_convention') &&
    !afw.includes('convention_readback') &&
    !afw.includes('LukeCalibrationCommands') &&
    !afw.includes('camera_convention_locked') &&
    producerCamera.includes('LukeProducerCameraAxes') &&
    !producerCamera.includes('forward_sign'),
    'One stable origin and typed title camera basis must serve plugin and every temporal camera without single-axis reflection');
console.log('DOOM right/down/forward conversion / shared stable origin / no runtime probe: PASS');
const depthConsistency = read('third_party/ofxr/src/d3d12/luke_depth_convert.hlsl');
const afwDepthOwner = read('third_party/ofxr/src/d3d12/luke_afw.cpp');
const depthSeed = depthConsistency.slice(depthConsistency.indexOf('float4 CalibrationSeed('),
    depthConsistency.indexOf('int2 SamplePixel('));
const depthCalibration = depthConsistency.slice(depthConsistency.indexOf('void CalibrateLukeDepth('),
    depthConsistency.indexOf('float RemappedDepth('));
const depthRemap = depthConsistency.slice(depthConsistency.indexOf('float RemappedDepth('),
    depthConsistency.indexOf('void ConvertLukeDepth('));
const depthConversion = depthConsistency.slice(depthConsistency.indexOf('void ConvertLukeDepth('));
assert(depthSeed.includes('current - prior') &&
    depthConsistency.includes('2 * Project(clip, true) - uv - Project(clip, false)') &&
    depthSeed.includes('length(observed - CameraDifference(uv, repaired))') &&
    depthSeed.includes('length(current - (Project(Clip(uv, repaired), true) - uv))') &&
    depthSeed.includes('DepthReprojectionTolerance') &&
    depthConsistency.includes('DepthReprojectionTolerance = .0015') &&
    depthConsistency.includes('CalibrationSamples = 1280') &&
    depthConsistency.includes('CalibrationSupport = 32') &&
    !depthConsistency.includes('CoherentDepth') && !depthConsistency.includes('support >= 4') &&
    !depthConsistency.includes('0.205') && !depthConsistency.includes('DOOM'),
    'Learned remap needs independent full-2D three-camera and native camera evidence, never object velocity, Y slack or a title offset');
assert(depthSeed.indexOf('NativeDepthConsistent(uv, depth, current)') < depthSeed.indexOf('float2 priorPixel') &&
    depthConsistency.includes('length(current - camera) <= DepthReprojectionTolerance') &&
    depthCalibration.includes('ResetCalibration != 0') &&
    depthCalibration.includes('saved.w >= float(CalibrationSupport)') &&
    depthCalibration.includes('NativeDepthConsistent(uv, repaired, current)') &&
    depthCalibration.includes('float4(saved.x, low, high, saved.w)') &&
    depthRemap.includes('depth >= calibration.y && depth <= calibration.z') &&
    depthRemap.includes('corrected > 0 && corrected < 1') &&
    depthRemap.includes('NativeDepthConsistent(uv, depth, NativeMotion(pixel))') &&
    depthConversion.includes('RemappedDepth(') &&
    !depthConversion.includes('CalibrationSeed(') && !depthConversion.includes('CameraDifference(') &&
    !depthConversion.includes('CurrentMotion.Load') && !depthConversion.includes('MiddleMotion.Load'),
    'Cache must freeze bias, extend only independently validated raw coverage, preserve native-consistent world and remove per-pixel refitting');
assert(afwDepthOwner.includes('depth_calibration_contracts[source_eye]') &&
    afwDepthOwner.includes('contract.epoch != guide.epoch') &&
    afwDepthOwner.includes('contract.projection.data(), guide.camera_view_to_clip.data()') &&
    afwDepthOwner.includes('depth_options.reset_calibration = depth_calibration_reset_pending ? 1U : 0U') &&
    afwDepthOwner.includes('list->SetPipelineState(depth_calibration_pipeline.Get())') &&
    afwDepthOwner.includes('calibration_ready.UAV.pResource = depth_remap_state.Get()') &&
    afwDepthOwner.indexOf('list->SetPipelineState(depth_calibration_pipeline.Get())') <
        afwDepthOwner.indexOf('list->SetPipelineState(depth_pipeline.Get())'),
    'One GPU calibration pass must precede conversion with a state UAV barrier and first/epoch/projection reset');
console.log('Stable measured GPU depth remap / full-2D evidence / retained native guard: PASS');
assert(afwDepthOwner.includes('const auto& native_previous = guide.previous_same_guide;') &&
    afwDepthOwner.includes('audit_middle_opposite_transform, native_previous_same_transform,') &&
    !afwDepthOwner.includes('previous_source_publication[source_eye] + 2 == guide.publication') &&
    afwDepthOwner.includes('queue->Wait(middle->producer_fence.Get(), middle->producer_fence_value)') &&
    afwDepthOwner.includes('std::swap(motion_before[i].Transition.StateBefore, motion_before[i].Transition.StateAfter)'),
    'depth estimator requires contiguous retained GPU-ordered guides and restores their states');
console.log('Three-publication stereo depth consistency / no title offset contract: PASS');
const originColor = read('third_party/ofxr/include/xrfg/luke_color_origin.hpp');
assert(originColor.includes('.2126f * red + .7152f * green + .0722f * blue') &&
    originColor.includes('luminance(channel(2), channel(1), channel(0))') &&
    originColor.includes('luminance(channel(0), channel(1), channel(2))') &&
    originColor.includes('luminance(half(channels[0]), half(channels[1]), half(channels[2]))') &&
    !originColor.includes('std::to_integer<unsigned char>(*pixel) / 255.f') &&
    originColor.includes('gradient_contrast = .001f') &&
    originColor.includes('edge_contrast = .002, bit_contrast = .001'),
    'Physically replace single-component origin decoding with RGB luminance; retain decoder thresholds');
console.log('RGB origin luminance / unchanged identity admission contract: PASS');
const originReaderSource = read('third_party/ofxr/src/d3d12/luke_color_origin.cpp');
const originReader = originReaderSource.slice(originReaderSource.indexOf('HRESULT read_luke_color_origin('));
const readerEvents = [...originReader.matchAll(/bridge_flight_logger\(\)\.event\(\s*(?:xrfg::)?BridgeFlightOperation::luke_afw_stage,\s*([^,]+),\s*(12[345]),\s*([\s\S]*?)\);/g)];
assert.deepEqual(readerEvents.map(event => Number(event[2])), [123, 124, 125],
    'Reader timing must consist of exactly three ordered RAM observations');
assert.deepEqual(readerEvents.map(event => event[1].trim()), ['result', 'result', 'decode_status'],
    'Timing results must reflect original Signal/wait status and final decoder HRESULT');
const readerTuple = event => event[3].replace(/\s/g, '');
assert(readerTuple(readerEvents[0]) === readerTuple(readerEvents[1]) &&
    readerTuple(readerEvents[1]) === readerTuple(readerEvents[2]) &&
    readerTuple(readerEvents[0]).includes('reader.get()') &&
    readerTuple(readerEvents[0]).endsWith(',value'),
    'Queue submission, GPU readiness and decoder completion must share exact reader/fence identity');
const signalAt = originReader.indexOf('queue->Signal(reader->fence.Get(), value)');
const retainedFenceAt = originReader.indexOf('reader->last_fence = value', signalAt);
const gpuWaitAt = originReader.indexOf('result = wait_reader(*reader, timeout_ms)', retainedFenceAt);
const mapAt = originReader.indexOf('reader->readback->Map(', gpuWaitAt);
const lumaAt = originReader.indexOf('luke_origin_luma(', mapAt);
const decodeAt = originReader.indexOf('decode_luke_origin_row(', lumaAt);
const unmapAt = originReader.indexOf('reader->readback->Unmap(', decodeAt);
assert(signalAt >= 0 && retainedFenceAt > signalAt && readerEvents[0].index > retainedFenceAt &&
    gpuWaitAt > readerEvents[0].index && readerEvents[1].index > gpuWaitAt &&
    mapAt > readerEvents[1].index && lumaAt > mapAt && decodeAt > lumaAt &&
    unmapAt > decodeAt && readerEvents[2].index > unmapAt,
    'Reader events must bracket only existing GPU wait and full CPU luma/decode/unmap work');
for (const [pattern, count] of [
    [/queue->Signal\(/g, 1], [/queue->ExecuteCommandLists\(/g, 1],
    [/reader->list->CopyTextureRegion\(/g, 1], [/wait_reader\(\*reader, timeout_ms\)/g, 2],
    [/reader->readback->Map\(/g, 1], [/reader->readback->Unmap\(/g, 1],
    [/decode_luke_origin_row\(/g, 1], [/luke_origin_luma\(/g, 1],
    [/CreateCommittedResource\(/g, 1], [/queue->Wait\(/g, 0],
]) assert.equal((originReader.match(pattern) ?? []).length, count,
    `Reader timing must not add submissions, resources, readbacks, waits or decode passes (${pattern})`);
assert.equal((originReader.match(/ram_enabled\(/g) ?? []).length, 1,
    'Reader timing must snapshot RAM observation state once');
assert(originReader.includes('const bool ram_reader_timing = bridge_flight_logger().ram_enabled()') &&
    (originReader.match(/if\s*\(\s*ram_reader_timing\s*\)\s*\{?\s*bridge_flight_logger\(\)\.event\(/g) ?? []).length === 3 &&
    (originReader.match(/\bram_reader_timing\b/g) ?? []).length === 4,
    'Each timing event, and only that event, must be RAM-only to avoid normal-log writes');
assert(!originReader.includes('generation_cooling_down') &&
    !originReader.includes('frame_generation_mode') && !originReader.includes('nvidia_new_route'),
    'Timing observations must not choose a gameplay route or depend on diagnostic mode');
console.log('Origin reader RAM-only wait/decode timing / no new GPU work or route: PASS');
const copySynthesizer = read('third_party/ofxr/src/d3d12/d3d12_frame_synthesizer.cpp');
const copyStart = copySynthesizer.indexOf('[[nodiscard]] HRESULT submit_copy(');
const copyEnd = copySynthesizer.indexOf('[[nodiscard]] HRESULT submit_prime(', copyStart);
assert(copyStart >= 0 && copyEnd > copyStart, 'Dedicated AFW color transport API must exist');
const copyTransport = copySynthesizer.slice(copyStart, copyEnd);
assert.equal((copyTransport.match(/->CopyResource\(/g) ?? []).length, 1,
    'AFW transport must record exactly one full-resource copy');
assert(copyTransport.includes('D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE') &&
    copyTransport.includes('D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON') &&
    copyTransport.includes('transition_barrier(destination, release_state,') &&
    copyTransport.includes('transition_barrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,'),
    'Copy-only transport must restore producer history and publication image states');
const copyWaitAt = copyTransport.indexOf('queue->Wait(next.producer_fence.Get(), current.fence_value)');
const copyExecuteAt = copyTransport.indexOf('queue->ExecuteCommandLists(1, lists)');
const copySignalAt = copyTransport.indexOf('queue->Signal(fence.Get(), fence_value)');
const copyRetireAt = copyTransport.indexOf('history->retire_consumer(next.lease, fence.Get(), fence_value)');
assert(copyWaitAt >= 0 && copyExecuteAt > copyWaitAt && copySignalAt > copyExecuteAt &&
    copyRetireAt > copySignalAt && copyTransport.includes('untracked_source = std::move(next)'),
    'Copy transport must retain its exact history lease until its signaled GPU completion, including signal failure');
assert(copyTransport.includes('previous.active()') &&
    copyTransport.includes('output_ticket->previous_serial = 0') &&
    copyTransport.includes('output_ticket->current_serial = current.serial') &&
    copyTransport.includes('output_ticket->synthetic_fence_value = fence_value') &&
    copyTransport.includes('output_ticket->synthetic_destination_index =') &&
    !copyTransport.includes('previous =') && !copyTransport.includes('Dispatch(') &&
    !copyTransport.includes('dispatch_fidelityfx(') && !copyTransport.includes('SetComputeRoot') &&
    !copyTransport.includes('record_prime(') && !copyTransport.includes('record_pair(') &&
    !copyTransport.includes('config.backend'),
    'Copy transport must create neither an implicit temporal anchor nor optical-flow/backend work');
const primeRecordStart = copySynthesizer.indexOf('[[nodiscard]] HRESULT record_prime(');
const primeRecordEnd = copySynthesizer.indexOf('[[nodiscard]] HRESULT dispatch_fidelityfx(', primeRecordStart);
const temporalPrimeRecord = copySynthesizer.slice(primeRecordStart, primeRecordEnd);
const temporalPrimeEnd = copySynthesizer.indexOf('[[nodiscard]] HRESULT submit_pair(', copyEnd);
const temporalPrime = copySynthesizer.slice(copyEnd, temporalPrimeEnd);
assert(primeRecordStart >= 0 && primeRecordEnd > primeRecordStart && temporalPrimeEnd > copyEnd &&
    temporalPrimeRecord.includes('pack_pipeline.Get()') && temporalPrimeRecord.includes('->Dispatch(') &&
    temporalPrimeRecord.includes('seed_index < 6U') && temporalPrimeRecord.includes('dispatch_fidelityfx(') &&
    temporalPrime.includes('record_prime(') && temporalPrime.includes('previous = std::move(next)'),
    'Real OFXR priming must retain its original pack, backend warm-up and rolling temporal anchor');
const afwCopyCall = negotiationOwnerSource.indexOf('submit_result = synthesizer->submit_copy(');
const afwCopyBranchStart = negotiationOwnerSource.lastIndexOf('if (afw_source) {', afwCopyCall);
const afwCopyBranchEnd = negotiationOwnerSource.indexOf('} else if (request_pair) {', afwCopyCall);
const afwCopyBranch = negotiationOwnerSource.slice(afwCopyBranchStart, afwCopyBranchEnd);
assert(afwCopyCall >= 0 && afwCopyBranchStart >= 0 && afwCopyBranchEnd > afwCopyCall &&
    (negotiationOwnerSource.match(/synthesizer->submit_copy\(/g) ?? []).length === 1 &&
    afwCopyBranch.includes('*capture, current_source_views,') &&
    !afwCopyBranch.includes('submit_prime(') && !afwCopyBranch.includes('submit_pair(') &&
    negotiationOwnerSource.includes('capture = afw_source->capture;') &&
    negotiationOwnerSource.includes('false, nullptr, true, nullptr, &*source.source_capture)') &&
    negotiationOwnerSource.includes('submit_result = synthesizer->submit_prime('),
    'All AFW input scratch-image transport must use its immutable copy-only source; genuine OFXR prime remains separate');
console.log('AFW immutable copy-only transport / asynchronous lease fence / real OFXR prime retained: PASS');
const startupPolicy = read('third_party/ofxr/include/xrfg/luke_afw_startup.hpp');
const startupKey = startupPolicy.slice(startupPolicy.indexOf('struct LukeAfwStartupKey {'),
    startupPolicy.indexOf('// A bounded startup rendezvous'));
assert(startupKey.includes('std::uint64_t stream{}') && startupKey.includes('std::uint64_t epoch{}') &&
    !startupKey.includes('revision') && !startupPolicy.includes('revision') &&
    startupPolicy.includes('stable_fresh_submissions = 3') &&
    startupPolicy.includes('maximum_attempts_per_key = 2') &&
    startupPolicy.includes('publication == last_publication_ + 1') &&
    startupPolicy.includes('completed_ = success') && startupPolicy.includes('void satisfy(') &&
    !startupPolicy.includes('Sleep(') && !startupPolicy.includes('std::chrono') &&
    !startupPolicy.includes('D3D12') && !startupPolicy.includes('DOOM'),
    'Startup policy must use only real producer stream/epoch, contiguous fresh evidence and bounded attempts, not UI revision or per-frame pacing');
const startupApplyStart = negotiationOwnerSource.indexOf('HRESULT apply_embedded_control(');
const startupApplyEnd = negotiationOwnerSource.indexOf('// Correlate source-release fingerprints', startupApplyStart);
const startupApply = negotiationOwnerSource.slice(startupApplyStart, startupApplyEnd);
assert(startupApplyStart >= 0 && startupApplyEnd > startupApplyStart &&
    startupApply.includes('bool force_startup_rendezvous = false') &&
    startupApply.includes('force_startup_rendezvous && control.desired.enabled) return E_ABORT') &&
    startupApply.includes('state->control_revision == control.revision && !force_startup_rendezvous') &&
    startupApply.includes('const bool changed = force_startup_rendezvous ||') &&
    startupApply.includes('const auto control = xrfg::embedded::snapshot()') &&
    startupApply.includes('state->menu_enabled = SUCCEEDED(result) && control.desired.enabled') &&
    startupApply.includes('return result;'),
    'Startup must force the existing GPU-safe reconfigure with the unchanged desired tuple and cancel a concurrent OFXR enable');
const startupScopeStart = negotiationOwnerSource.indexOf('auto startup_request = state->luke_afw_startup.pending();');
const startupScopeEnd = negotiationOwnerSource.indexOf('if (state->luke_afw_previous_selected &&', startupScopeStart);
const startupScope = negotiationOwnerSource.slice(startupScopeStart, startupScopeEnd);
assert(startupScopeStart >= 0 && startupScopeEnd > startupScopeStart &&
    startupScope.includes('if (!afw_selected || desired_control.desired.enabled)') &&
    startupScope.includes('else if (startup_request && !luke_afw_released_eye)') &&
    startupScope.includes('startup_request.reset()') &&
    startupScope.includes('apply_embedded_control(state, startup_request.has_value())') &&
    startupScope.includes('complete(*startup_request, SUCCEEDED(control_result))') &&
    startupScope.includes('afw_selected && !state->menu_enabled') &&
    startupScope.includes('if (!physical_success)') && startupScope.includes('else if (!held)') &&
    startupScope.includes('observe_fresh(candidate->key, candidate->publication)') &&
    startupScope.includes('satisfy(candidate->key)') &&
    startupScope.includes('state.luke_afw_startup_drained == candidate->key') &&
    startupScope.includes('state->luke_afw_startup_drained = state->luke_afw_last_delivered_startup_key') &&
    !startupScope.includes('startup_request->revision') && !startupScope.includes('candidate->key.revision') &&
    /startup_request && xrfg::bridge_flight_logger\(\)\.ram_enabled\(\)[\s\S]*?luke_frame_contract,[\s\S]*?S_OK, 126,/.test(startupScope) &&
    /if \(xrfg::bridge_flight_logger\(\)\.ram_enabled\(\)\)[\s\S]*?luke_frame_contract,[\s\S]*?control_result, 127,/.test(startupScope),
    'Only AFW-selected/OFXR-Off fresh physical success may arm; held is ignored, failures cancel, ordinary hot drain satisfies and receipts remain RAM-only');
const startupPhysical = negotiationOwnerSource.indexOf('startup_observation.physical_success = !pair_ready &&');
const startupReject = negotiationOwnerSource.lastIndexOf('if (XR_FAILED(result)) {', startupPhysical);
assert(startupPhysical > startupReject && startupReject >= 0 &&
    negotiationOwnerSource.slice(startupReject, startupPhysical).includes('return result;') &&
    negotiationOwnerSource.slice(startupPhysical, startupPhysical + 180).includes('prepared.kind == PreparedGenerationKind::prime') &&
    negotiationOwnerSource.includes('startup_observation.candidate = afw_produced.startup_candidate') &&
    negotiationOwnerSource.includes('startup_observation.held = afw_produced.held_afw_pair != nullptr') &&
    /output\.startup_candidate = LukeAfwStartupCandidate\{\s*\{source\.guide->stream, source\.guide->epoch\},\s*source\.guide->publication\}/.test(negotiationOwnerSource) &&
    (negotiationOwnerSource.match(/luke_afw_startup\.observe_fresh\(/g) ?? []).length === 1,
    'One immutable fresh producer candidate must qualify only after accepted physical XR completion, never an async OFXR handoff');
console.log('Bounded startup rendezvous / real stream-epoch key / physical Off-only evidence / RAM receipts: PASS');
