//========================================================================
// Copyright (c) Yann Clotioloman Yeo, 2018
//
//	Author					: Yann Clotioloman Yeo
//	E-Mail					: kronosrenderer@gmail.com
//========================================================================

#include "stdafx.h"

#include "Graphics/Light/DirectionnalLight.h"
#include "Graphics/Light/OmniLight.h"
#include "Graphics/Material/DefaultDielectric.h"
#include "Graphics/Material/DefaultMetal.h"
#include "Graphics/Renderer/Realtime/Dx12/Dx12Renderer.h"
#include "ForwardLighning.h"
#include <dxgi1_4.h>
#include <minwinbase.h>

namespace Graphics { namespace Renderer { namespace Realtime { namespace Dx12 { namespace Effect
{
using namespace DirectX;

ForwardLighning::ForwardLighning(const DXGI_SAMPLE_DESC& sampleDesc)
: BaseEffect(sampleDesc)
{
	initRootSignature();
	initPipelineStateObjects();
	initStaticConstantBuffers();

	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		m_pixelShaderMaterialCBUploadHeaps[i] = nullptr;
		m_pixelShaderMaterialCBDefaultHeaps[i] =  nullptr;
	}
}

ForwardLighning::~ForwardLighning()
{
	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		if (m_pixelShaderMaterialCBUploadHeaps[i])
			m_pixelShaderMaterialCBUploadHeaps[i]->Release();

		if (m_pixelShaderMaterialCBDefaultHeaps[i])
			m_pixelShaderMaterialCBDefaultHeaps[i]->Release();
	}
}

void ForwardLighning::onUpdateMaterial(const Scene::BaseScene& scene, const MaterialIdentifier& matId, ID3D12GraphicsCommandList* commandList)
{
	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_pixelShaderMaterialCBDefaultHeaps[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
	}

	updateMaterial(scene, matId, commandList);

	fromMaterialUploadHeapToDefaulHeap(commandList);
}

void ForwardLighning::initRootSignature()
{
	D3D12_ROOT_PARAMETER rootParameters[6];

	// A root descriptor, which explains where to find the data for the parameter
	D3D12_ROOT_DESCRIPTOR rootCBVDescriptor;
	rootCBVDescriptor.RegisterSpace = 0;
	rootCBVDescriptor.ShaderRegister = 0;

	// 0 : Root parameter for the vertex shader constant buffer
	kInt32 paramIdx = 0;
	rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // this is a constant buffer view root descriptor
	rootParameters[paramIdx].Descriptor = rootCBVDescriptor; // this is the root descriptor for this root parameter
	rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX; // our vertex shader will be the only shader accessing this parameter for now
	++paramIdx;

	// 1 & 2 : Root parameter for the light and material pixel shader constant buffer
	for (kInt32 i = 0; i < 2; ++i, ++paramIdx)
	{
		rootCBVDescriptor.ShaderRegister = i;
		rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameters[paramIdx].Descriptor = rootCBVDescriptor;
		rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	}

	// 3 & 4 & 5 Root parameter for the pixel shader textures.
	// create a descriptor range and fill it out
	// this is a range of descriptors inside a descriptor heap
	static const kInt32 nbTextures = 3;
	D3D12_DESCRIPTOR_RANGE descriptorTableRanges[nbTextures];

	for (kInt32 i = 0; i < nbTextures; ++i, ++paramIdx)
	{
		descriptorTableRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; // this is a range of shader resource views (descriptors)
		descriptorTableRanges[i].NumDescriptors = 1;
		descriptorTableRanges[i].RegisterSpace = 0; // space 0. can usually be zero
		descriptorTableRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND; // this appends the range to the end of the root signature descriptor tables
		descriptorTableRanges[i].BaseShaderRegister = i; // start index of the shader registers in the range
		
		// create a descriptor table
		D3D12_ROOT_DESCRIPTOR_TABLE descriptorTable;
		descriptorTable.NumDescriptorRanges = 1; // we only have one range
		descriptorTable.pDescriptorRanges = &descriptorTableRanges[i]; // the pointer to the beginning of our ranges array

		// fill out the parameter for our descriptor table. Remember it's a good idea to sort parameters by frequency of change. Our constant
		// buffer will be changed multiple times per frame, while our descriptor table will not be changed at all (in this tutorial)
		rootParameters[paramIdx].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // this is a descriptor table
		rootParameters[paramIdx].DescriptorTable = descriptorTable; // this is our descriptor table for this root parameter
		rootParameters[paramIdx].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; // our pixel shader will be the only shader accessing this parameter for now
	}

	// create root signature
	createRootSignature(rootParameters, _countof(rootParameters));
}

void ForwardLighning::initPipelineStateObjects()
{
	// create input layout
	D3D12_INPUT_ELEMENT_DESC inputLayoutElement[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "BITANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	// The blending description
	D3D12_BLEND_DESC blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	blendDesc.RenderTarget[0].BlendEnable = TRUE;
	blendDesc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
	blendDesc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	blendDesc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	blendDesc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// Depth stencil description
	D3D12_DEPTH_STENCIL_DESC dsDesc = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	dsDesc.DepthEnable = TRUE;
	dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

	dsDesc.StencilEnable = TRUE;
	dsDesc.StencilReadMask = 0xFF;
	dsDesc.StencilWriteMask = 0xFF;

	// Front-facing pixels.
	dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER;

	// Back-facing pixels.
	dsDesc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER;

	// Compile vertex shader
	ID3DBlob* vertexShader = compileShader(std::wstring(L"ForwardLightning_VS.hlsl"), true);

	// Pixel shader macros
	auto maxLightDef = std::to_string(Graphics::Light::MaxLightsPerScene);
	auto defaultDielectricIdxDef = std::to_string(Material::BaseMaterial::Type::DefaultDieletric);
	auto defaultMetalIdxDef = std::to_string(Material::BaseMaterial::Type::DefaultMetal);
	auto perfectMirrorIdxDef = std::to_string(Material::BaseMaterial::Type::PerfectMirror);
	auto plasticIdxDef = std::to_string(Material::BaseMaterial::Type::Plastic);

	D3D_SHADER_MACRO macros[] = {	"MAX_LIGHTS", maxLightDef.c_str(),
									"DEFAULT_DIELECTRIC_IDX", defaultDielectricIdxDef.c_str(),
									"DEFAULT_METAL_IDX", defaultMetalIdxDef.c_str(),
									"PERFECT_MIRROR_IDX", perfectMirrorIdxDef.c_str(),
									"PLASTIC_IDX", plasticIdxDef.c_str(), NULL, NULL
	};

	// Compile vertex shader
	ID3DBlob* pixelShader = compileShader(std::wstring(L"ForwardLightning_PS.hlsl"), false, macros);

	// Compile solid PSO
	compilePipeline(m_solidPSO,
		vertexShader,
		pixelShader,
		inputLayoutElement,
		sizeof(inputLayoutElement) / sizeof(D3D12_INPUT_ELEMENT_DESC),
		blendDesc,
		dsDesc
	);
	
	m_solidPSO->SetName(L"Forward lightning solid PSO");

	// Compile wireframe PSO
	const CD3DX12_RASTERIZER_DESC rasterizerDesc(D3D12_FILL_MODE_WIREFRAME, D3D12_CULL_MODE_NONE,
		FALSE /* FrontCounterClockwise */,
		D3D12_DEFAULT_DEPTH_BIAS,
		D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
		D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS,
		TRUE /* DepthClipEnable */,
		TRUE /* MultisampleEnable */,
		FALSE /* AntialiasedLineEnable */,
		0 /* ForceSampleCount */,
		D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF);

	compilePipeline(m_wireframePSO,
		vertexShader,
		pixelShader,
		inputLayoutElement,
		sizeof(inputLayoutElement) / sizeof(D3D12_INPUT_ELEMENT_DESC),
		blendDesc,
		dsDesc,
		rasterizerDesc
	);

	m_wireframePSO->SetName(L"Forward lightning wireframe PSO");
}

void ForwardLighning::initStaticConstantBuffers()
{
	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		// 0 : Vertex shader constant Buffer Upload Resource Heap
		HRESULT hr = D3d12Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // this heap will be used to upload the constant buffer data
			D3D12_HEAP_FLAG_NONE, // no flags
			&CD3DX12_RESOURCE_DESC::Buffer(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT), // size of the resource heap. Must be a multiple of 64KB for single-textures and constant buffers
			D3D12_RESOURCE_STATE_GENERIC_READ, // will be data that is read from so we keep it in the generic read state
			nullptr, // we do not have use an optimized clear value for constant buffers
			IID_PPV_ARGS(&m_vertexShaderCBUploadHeaps[i]));

		KRONOS_ASSERT(SUCCEEDED(hr));

		m_vertexShaderCBUploadHeaps[i]->SetName(L"Vertex shader Constant Buffer Upload heap");
		KRONOS_ASSERT(SUCCEEDED(hr));

		// map the resource heap to get a gpu virtual address to the beginning of the heap
		hr = m_vertexShaderCBUploadHeaps[i]->Map(0, &readRangeGPUOnly, reinterpret_cast<void**>(&m_vertexShaderCBGPUAddress[i]));
		KRONOS_ASSERT(SUCCEEDED(hr));
		
		// 1 : Pixel shader lights constant Buffer Upload Resource Heap
		hr = D3d12Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderLightCBAlignedSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_pixelShaderLightsCBUploadHeaps[i]));

		KRONOS_ASSERT(SUCCEEDED(hr));
		m_pixelShaderLightsCBUploadHeaps[i]->SetName(L"Pixel shader lights Constant Buffer Upload heap");

		hr = m_pixelShaderLightsCBUploadHeaps[i]->Map(0, &readRangeGPUOnly, reinterpret_cast<void**>(&m_pixelShaderLightsCBGPUAddress[i]));
		KRONOS_ASSERT(SUCCEEDED(hr));
	}
}

void ForwardLighning::initDynamicMaterialConstantBuffer(const Scene::BaseScene& scene, ID3D12GraphicsCommandList* commandList)
{
	const auto& materials = scene.getModel()->getMaterials();
	if (!materials.size())
		return;

	// Allocate the maximal possible number of materials.
	// This allow material references to be still be valid during deletion.
	const kUint32 bufferSize = (--materials.end())->second->getId() + 1;

	if (bufferSize > m_materialBufferSize)
	{
		m_materialBufferSize = bufferSize;

		// 0 : Init heaps
		for (kInt32 i = 0; i < swapChainBufferCount; ++i)
		{
			if (m_pixelShaderMaterialCBUploadHeaps[i])
				m_pixelShaderMaterialCBUploadHeaps[i]->Release();

			if (m_pixelShaderMaterialCBDefaultHeaps[i])
				m_pixelShaderMaterialCBDefaultHeaps[i]->Release();

			// Create upload heap
			HRESULT hr = D3d12Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderMaterialCBAlignedSize * bufferSize),
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&m_pixelShaderMaterialCBUploadHeaps[i]));

			KRONOS_ASSERT(SUCCEEDED(hr));
			m_pixelShaderMaterialCBUploadHeaps[i]->SetName(L"Pixel shader material Constant Buffer Upload heap");

			hr = m_pixelShaderMaterialCBUploadHeaps[i]->Map(0, &readRangeGPUOnly, reinterpret_cast<void**>(&m_pixelShaderMaterialCBGPUAddress[i]));
			KRONOS_ASSERT(SUCCEEDED(hr));

			// Create default heap
			hr = D3d12Device->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&CD3DX12_RESOURCE_DESC::Buffer(PixelShaderMaterialCBAlignedSize * bufferSize),
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_pixelShaderMaterialCBDefaultHeaps[i]));

			KRONOS_ASSERT(SUCCEEDED(hr));
			m_pixelShaderMaterialCBDefaultHeaps[i]->SetName(L"Pixel shader material Constant Buffer Default Resource heap");
		}
	}

	// 1 : Update maerials in upload heap
	for (auto& mat : materials)
	{
		updateMaterial(scene, mat.first, commandList);
	}

	// 2: Copy to default heap
	fromMaterialUploadHeapToDefaulHeap(commandList);
}

void ForwardLighning::fromMaterialUploadHeapToDefaulHeap(ID3D12GraphicsCommandList* commandList)
{
	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		// Copy upload heap to default
		commandList->CopyResource(m_pixelShaderMaterialCBDefaultHeaps[i], m_pixelShaderMaterialCBUploadHeaps[i]);

		// Transition to pixel shader resource.
		commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_pixelShaderMaterialCBDefaultHeaps[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	}
}

void ForwardLighning::updateMaterial(const Scene::BaseScene& scene, const MaterialIdentifier& matId, ID3D12GraphicsCommandList* commandList)
{
	using namespace Graphics::Material;

	const DX12Model* dx12Model = static_cast<const DX12Model*>(scene.getModel().get());
	auto materialHandle = dx12Model->getMaterialHandle(matId);

	for (kInt32 i = 0; i < swapChainBufferCount; ++i)
	{
		PixelShaderMaterialCB pixelShaderMaterialCB;
		auto& materialCB = pixelShaderMaterialCB.material;
		ZeroMemory(&pixelShaderMaterialCB, sizeof(pixelShaderMaterialCB));

		const BaseMaterial* material = dx12Model->getMaterial(materialHandle->matId);
		if (material->isFresnelMaterial())
		{
			const FresnelMaterial* fresnelMat = static_cast<const FresnelMaterial*>(material);

			materialCB.ambient = { fresnelMat->getAmbient().r, fresnelMat->getAmbient().g, fresnelMat->getAmbient().b, 1.0f };
			materialCB.shininess = fresnelMat->getShininess();
			materialCB.roughness = fresnelMat->getRoughness();
			materialCB.fresnel0 = fresnelMat->getFresnel0();
		}
		else
		{
			materialCB.diffuse = { defaultAmbient, defaultAmbient, defaultAmbient, 1.0f };
			materialCB.ambient = { defaultAmbient, defaultAmbient, defaultAmbient, 1.0f };
		}

		if (material->isDielectric())
		{
			const DefaultDielectric* dielectricMat = static_cast<const DefaultDielectric*>(material);
			materialCB.diffuse = { dielectricMat->getDiffuse().r, dielectricMat->getDiffuse().g, dielectricMat->getDiffuse().b, 1.0f };
			materialCB.specular = { dielectricMat->getSpecular().r, dielectricMat->getSpecular().g, dielectricMat->getSpecular().b, 1.0f };
			materialCB.emissive = { dielectricMat->getEmissive().r, dielectricMat->getEmissive().g, dielectricMat->getEmissive().b, 1.0f };
		}
		else if (material->getType() == BaseMaterial::Type::DefaultMetal)
		{
			const DefaultMetal* metalMat = static_cast<const DefaultMetal*>(material);
			materialCB.diffuse = { metalMat->getReflectance().r, metalMat->getReflectance().g, metalMat->getReflectance().b, 1.0f };
		}

		materialCB.opacity = material->getOpacity();
		materialCB.type = static_cast<INT>(material->getType());

		materialCB.hasDiffuseTex = materialHandle->diffuseTexture.isValid();
		materialCB.hasSpecularTex = materialHandle->specularTexture.isValid();
		materialCB.hasNormalTex = materialHandle->normalTexture.isValid();

		kUint64 posInCB = matId * PixelShaderMaterialCBAlignedSize;
		memcpy(m_pixelShaderMaterialCBGPUAddress[i] + posInCB, &pixelShaderMaterialCB, sizeof(PixelShaderMaterialCB));
	}
}

void ForwardLighning::updateVertexShaderCB(ForwardLightningPushArgs& data, kInt32 frameIndex)
{
	VertexShaderCB vertexShaderCB;


	XMStoreFloat4x4(&vertexShaderCB.wvpMat,
		data.scene.getCamera()->getDirectXTransposedMVP());

	memcpy(m_vertexShaderCBGPUAddress[frameIndex], &vertexShaderCB, sizeof(VertexShaderCB));
}

void ForwardLighning::updatePixelShaderLightsCB(ForwardLightningPushArgs& data, kInt32 frameIndex)
{
	ZeroMemory(&m_pixelShaderLightsCB, sizeof(PixelShaderEnvironmentCb));

	if (const auto& media = data.scene.getMedia())
	{
		m_pixelShaderLightsCB.mediaInfo = { static_cast<FLOAT>(!!media),
			1.0f - media->getExtinctionCoeff(),
			media->getExtinctionCoeffTweaked(),
			1.0f };
	}

	const auto& lights = data.scene.getLights();

	const kUint32 lightSize = static_cast<kUint32>(lights.size());
	m_pixelShaderLightsCB.nbLights = lightSize;

	const glm::vec3 cameraPos = data.scene.getCamera().get()->getPosition();
	m_pixelShaderLightsCB.eyePosition = { cameraPos.x, cameraPos.y, cameraPos.z, 0.0f};

	const auto& ambientColor = data.scene.getAmbientColor();
	m_pixelShaderLightsCB.sceneAmbient = { ambientColor.x, ambientColor.y, ambientColor.z, 0.0f};

	kInt32 pos = 0;

	for (auto& lightIter : lights)
	{
		auto& dx12Light = m_pixelShaderLightsCB.lights[pos];
		auto lightType = lightIter.second->getType();

		dx12Light.type = static_cast<kInt32>(lightType);

		if (lightIter.second->getType() == Graphics::Light::LightType::Point)
		{
			auto* omniLight = static_cast<Graphics::Light::OmniLight*>(lightIter.second.get());

			const auto& pos = omniLight->getPosition();
			dx12Light.position = { pos.x, pos.y, pos.z, 0.0f };
			dx12Light.range = omniLight->getRange();
		}
		else if (lightIter.second->getType() == Graphics::Light::LightType::Directionnal)
		{
			auto* directionnalLight = static_cast<Graphics::Light::DirectionnalLight*>(lightIter.second.get());
			const auto& direction = directionnalLight->getNormalizedDirection();

			dx12Light.direction = { direction.x, direction.y, direction.z, 0.0f };
		}

		const auto& color = lightIter.second->getFinalColor();
		dx12Light.color = { color.r, color.g, color.r, 0.0f };

		++pos;
	}

	memcpy(m_pixelShaderLightsCBGPUAddress[frameIndex], &m_pixelShaderLightsCB, sizeof(PixelShaderEnvironmentCb));
}

void ForwardLighning::pushDrawCommands(ForwardLightningPushArgs& data, ID3D12GraphicsCommandList* commandList, kInt32 frameIndex)
{
	// Set Pso
	commandList->SetPipelineState(data.scene.isWireframeEnabled() ? m_wireframePSO : m_solidPSO);

	// set root signature
	commandList->SetGraphicsRootSignature(m_rootSignature);

	// set the primitive topology
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// update shared constant buffers
	updateVertexShaderCB(data, frameIndex);
	updatePixelShaderLightsCB(data, frameIndex);

	// set shared constant buffer views
	commandList->SetGraphicsRootConstantBufferView(0, m_vertexShaderCBUploadHeaps[frameIndex]->GetGPUVirtualAddress());
	commandList->SetGraphicsRootConstantBufferView(1, m_pixelShaderLightsCBUploadHeaps[frameIndex]->GetGPUVirtualAddress());

	const auto* dx12Model = static_cast<const DX12Model*>(data.scene.getModel().get());
	const auto& meshByGroup = dx12Model->getMeshesByGroup();

	auto& getPSReadyTextureHandle = [dx12Model, commandList](const ImageIdentifier& imageId, const DX12Model::SharedTMaterialHandlePtr& materialHandle, kInt32 rootParamIdx)
	{
		const auto tex = imageId.isValid() ? dx12Model->getTexture(imageId) : nullptr;
		if (tex)
		{
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(tex->getHandle().buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
			commandList->SetGraphicsRootDescriptorTable(rootParamIdx, tex->getHandle().descriptorHandle.getGpuHandle());
		}

		return tex;
	};

	for (auto& group : dx12Model->getMeshHandlesByGroup())
	{
		const auto materialHandle = dx12Model->getMaterialHandle(
			meshByGroup.find(group.first)->second[0]->m_materialId
		);

		const kUint32 matPosInCB = materialHandle->matId * PixelShaderMaterialCBAlignedSize;
		commandList->SetGraphicsRootConstantBufferView(2, m_pixelShaderMaterialCBDefaultHeaps[frameIndex]->GetGPUVirtualAddress() + matPosInCB);
		
		// Set textures states.
		const auto diffuseTex = getPSReadyTextureHandle(materialHandle->diffuseTexture, materialHandle, 3);
		const auto specularTex = getPSReadyTextureHandle(materialHandle->specularTexture, materialHandle, 4);
		const auto normalTex = getPSReadyTextureHandle(materialHandle->normalTexture, materialHandle, 5);

		// Draw meshes.
		for (auto* mesh : group.second)
		{
			commandList->IASetVertexBuffers(0, 1, &mesh->vertexBuffer.bufferView);
			commandList->IASetIndexBuffer(&mesh->indexBuffer.bufferView);

			commandList->DrawIndexedInstanced(mesh->nbIndices, 1, 0, 0, 0);
		}

		// Reset texture states.
		if (diffuseTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(diffuseTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
		
		if (specularTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(specularTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
	
		if (normalTex)
			commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(normalTex->getHandle().buffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON));
	}
}
}}}}}
