#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AssimpLoader.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/texture.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <Windows.h>
#include <cctype>
#include <cmath>
#include <limits>
#include <system_error>

using namespace DirectX;

namespace
{
	// 셰이더와 FrameResource의 BoneTransforms 배열 크기와 반드시 맞춰야 한다.
	constexpr UINT MaxSkinnedBones = 256;

	// FBX 재질/애니메이션 이름에 경로 문자나 공백이 섞여도 map key로 안전하게 쓰도록 바꾼다.
	std::string MakeSafeName(const std::string& name, const std::string& fallback)
	{
		std::string result;
		result.reserve(name.size());

		for(char ch : name)
		{
			const unsigned char value = static_cast<unsigned char>(ch);
			if(std::isalnum(value) || ch == '_' || ch == '-')
				result.push_back(ch);
			else if(ch == ' ' || ch == '.' || ch == ':' || ch == '\\' || ch == '/')
				result.push_back('_');
		}

		return result.empty() ? fallback : result;
	}

	// animation key 시간을 한 배열에 모은 뒤 정렬해서 bone별 keyframe을 만든다.
	void AddUniqueTime(std::vector<double>& times, double time)
	{
		if(time < 0.0)
			return;

		times.push_back(time);
	}

	// 같은 시간대의 position/rotation/scale key가 중복으로 들어오는 것을 제거한다.
	void SortAndUniqueTimes(std::vector<double>& times)
	{
		std::sort(times.begin(), times.end());
		times.erase(std::unique(times.begin(), times.end(),
			[](double lhs, double rhs)
			{
				return std::abs(lhs - rhs) < 0.00001;
			}), times.end());
	}

	std::string GetFileNameOnly(std::string path)
	{
		std::replace(path.begin(), path.end(), '\\', '/');

		const size_t slash = path.find_last_of('/');
		if(slash == std::string::npos)
			return path;

		return path.substr(slash + 1);
	}

	bool HasExtension(const std::string& filename)
	{
		const size_t dot = filename.find_last_of('.');
		return dot != std::string::npos && dot + 1 < filename.size();
	}

	std::string SanitizeFileName(std::string filename)
	{
		const std::string invalidChars = "<>:\"/\\|?*";

		for(char& ch : filename)
		{
			if(invalidChars.find(ch) != std::string::npos)
				ch = '_';
		}

		return filename;
	}

	std::string EmbeddedTextureExtension(const aiTexture* texture)
	{
		std::string extension = texture != nullptr ? texture->achFormatHint : "";

		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char ch)
			{
				return (char)std::tolower(ch);
			});

		if(extension.empty())
			extension = "png";

		if(extension.front() == '.')
			extension.erase(extension.begin());

		if(extension == "jpeg")
			extension = "jpg";

		return extension;
	}

	std::string PathToString(const std::filesystem::path& path)
	{
		return path.string();
	}

	std::string SaveEmbeddedTextureToFile(
		const aiScene* scene,
		const aiString& texturePath,
		const std::filesystem::path& outputFolder,
		const std::string& fallbackName)
	{
		if(scene == nullptr)
			return "";

		const aiTexture* embeddedTex = scene->GetEmbeddedTexture(texturePath.C_Str());
		if(embeddedTex == nullptr)
			return "";

		if(embeddedTex->mHeight != 0)
		{
			OutputDebugStringA("Raw embedded textures are not handled yet\n");
			return "";
		}

		if(embeddedTex->mWidth == 0 || embeddedTex->pcData == nullptr)
			return "";

		std::error_code error;
		std::filesystem::create_directories(outputFolder, error);
		if(error)
		{
			OutputDebugStringA("Failed to create embedded texture output folder\n");
			return "";
		}

		const std::string extension = EmbeddedTextureExtension(embeddedTex);
		std::string fileName = GetFileNameOnly(texturePath.C_Str());

		if(fileName.empty() || fileName.find('*') != std::string::npos)
			fileName = fallbackName + "." + extension;
		else if(!HasExtension(fileName))
			fileName += "." + extension;

		fileName = SanitizeFileName(fileName);

		const std::filesystem::path finalPath = outputFolder / fileName;
		std::ofstream file(finalPath, std::ios::binary);
		if(!file.is_open())
		{
			OutputDebugStringA("Failed to save embedded texture\n");
			return "";
		}

		file.write(
			reinterpret_cast<const char*>(embeddedTex->pcData),
			static_cast<std::streamsize>(embeddedTex->mWidth));

		if(!file.good())
		{
			OutputDebugStringA("Failed while writing embedded texture\n");
			return "";
		}

		OutputDebugStringW(L"Saved embedded texture: ");
		OutputDebugStringW(finalPath.wstring().c_str());
		OutputDebugStringW(L"\n");

		return PathToString(finalPath);
	}

	std::string ResolveExternalTexturePath(
		const std::string& texturePath,
		const std::string& modelDirectory)
	{
		if(texturePath.empty())
			return "";

		std::error_code error;
		const std::filesystem::path sourcePath(texturePath);
		if(sourcePath.is_absolute() && std::filesystem::exists(sourcePath, error))
			return PathToString(sourcePath);

		const std::string filename = GetFileNameOnly(texturePath);
		if(filename.empty())
			return "";

		const std::filesystem::path modelTexturePath = std::filesystem::path(modelDirectory) / filename;
		error.clear();
		if(std::filesystem::exists(modelTexturePath, error))
			return PathToString(modelTexturePath);

		return filename;
	}
}

bool AssimpLoader::LoadFbx(
	const std::string& filename,
	std::vector<M3DLoader::SkinnedVertex>& vertices,
	std::vector<std::uint32_t>& indices,
	std::vector<M3DLoader::Subset>& subsets,
	std::vector<M3DLoader::M3dMaterial>& mats,
	SkinnedData& skinInfo)
{
	// LoadFbx를 다시 호출해도 이전 FBX 데이터가 섞이지 않도록 출력과 내부 상태를 먼저 비운다.
	vertices.clear();
	indices.clear();
	subsets.clear();
	mats.clear();

	// 여러 보조 함수가 같은 출력 배열에 채워 넣기 때문에 이번 호출 동안만 포인터로 잡아 둔다.
	mVertices = &vertices;
	mIndices = &indices;
	mSubsets = &subsets;
	mMats = &mats;

	mBoneNameToIndex.clear();
	mBoneOffsetByName.clear();
	mBoneBindPoseByName.clear();
	mBoneNames.clear();
	mBoneOffsets.clear();
	mBoneHierarchy.clear();
	mAnimations.clear();
	mVertexWeights.clear();
	mVertexBoneIndices.clear();
	mPendingWeights.clear();
	mLastError.clear();
	const std::filesystem::path modelPath(filename);
	mModelDirectory = modelPath.has_parent_path() ? PathToString(modelPath.parent_path()) : ".";

	// 실패/성공 어느 쪽이든 함수가 끝나면 외부 배열 포인터를 반드시 끊는다.
	auto clearOutputPointers = [this]()
	{
		mVertices = nullptr;
		mIndices = nullptr;
		mSubsets = nullptr;
		mMats = nullptr;
	};

	Assimp::Importer importer;

	// PreTransformVertices를 쓰면 bone 계층이 사라지므로 스키닝 FBX에서는 사용하면 안 된다.
	const UINT importFlags =
		aiProcess_Triangulate |
		aiProcess_ConvertToLeftHanded |
		aiProcess_GenSmoothNormals |
		aiProcess_CalcTangentSpace |
		aiProcess_LimitBoneWeights;

	const aiScene* scene = importer.ReadFile(filename, importFlags);
	if(scene == nullptr || scene->mRootNode == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
	{
		mLastError = importer.GetErrorString();
		if(mLastError.empty())
			mLastError = "Assimp failed to load the FBX scene.";

		clearOutputPointers();
		return false;
	}

	if(!ProcessNode(scene->mRootNode, scene))
	{
		clearOutputPointers();
		return false;
	}

	// mesh가 하나도 없으면 이후 vertex/index 버퍼를 만들 수 없으므로 여기서 중단한다.
	if(vertices.empty() || indices.empty())
	{
		mLastError = "The FBX file did not contain a triangle mesh.";
		clearOutputPointers();
		return false;
	}

	// 본이 직접 weight를 가진 노드뿐 아니라 그 부모 노드도 계층에 넣어야 bind pose가 무너지지 않는다.
	// bone weight가 직접 달린 노드뿐 아니라 부모 노드까지 포함해야 bind pose가 유지된다.
	BuildBoneHierarchy(scene->mRootNode, -1);
	AddMissingBones();

	// bone이 없는 정적 FBX도 스키닝 경로로 그릴 수 있게 최소 root bone을 만든다.
	if(mBoneNames.empty())
	{
		mBoneBindPoseByName["Root"] = LocalTransform();
		CreateBone("Root", -1);
	}

	if(mBoneNames.size() > MaxSkinnedBones)
	{
		mLastError = "The FBX contains more than 256 bones, but the current shader constant buffer only supports 256.";
		clearOutputPointers();
		return false;
	}

	// vertex weight를 최종 bone index로 바꾸고, FBX animation channel을 SkinnedData로 변환한다.
	ApplyPendingBoneWeights();
	ReadAnimations(scene);

	// 애니메이션이 없는 FBX도 멈춘 bind pose 클립으로 렌더링할 수 있게 Take1을 만든다.
	if(mAnimations.empty())
	{
		AnimationClip bindPoseClip;
		bindPoseClip.BoneAnimations.resize(mBoneNames.size());

		for(UINT i = 0; i < (UINT)mBoneNames.size(); ++i)
			bindPoseClip.BoneAnimations[i] = BuildStaticBoneAnimation(mBoneNames[i], 1.0f / 30.0f);

		mAnimations["Take1"] = bindPoseClip;
	}
	else if(mAnimations.find("Take1") == mAnimations.end())
	{
		// 기존 렌더 코드가 Take1을 재생하므로 첫 애니메이션을 Take1이라는 별칭으로도 보관한다.
		AnimationClip firstClip = mAnimations.begin()->second;
		mAnimations["Take1"] = firstClip;
	}

	skinInfo.Set(mBoneHierarchy, mBoneOffsets, mAnimations);
	clearOutputPointers();

	return true;
}

bool AssimpLoader::ProcessNode(const aiNode* node, const aiScene* scene)
{
	// FBX 노드는 여러 mesh를 가질 수 있으므로 현재 노드의 mesh부터 모두 변환한다.
	for(UINT i = 0; i < node->mNumMeshes; ++i)
	{
		const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
		if(!ProcessMesh(mesh, scene))
			return false;
	}

	// 자식 노드도 같은 방식으로 순회해서 FBX 전체 장면의 mesh를 가져온다.
	for(UINT i = 0; i < node->mNumChildren; ++i)
	{
		if(!ProcessNode(node->mChildren[i], scene))
			return false;
	}

	return true;
}

bool AssimpLoader::ProcessMesh(const aiMesh* mesh, const aiScene* scene)
{
	if(mesh == nullptr || mesh->mNumVertices == 0 || mesh->mNumFaces == 0)
		return true;

	// 여러 mesh를 하나의 vertex/index buffer에 이어 붙이므로 시작 위치를 기록해 둔다.
	const UINT baseVertex = (UINT)mVertices->size();
	const UINT baseIndex = (UINT)mIndices->size();
	const UINT subsetIndex = (UINT)mSubsets->size();

	M3DLoader::Subset subset;
	subset.Id = subsetIndex;
	subset.VertexStart = baseVertex;
	subset.VertexCount = mesh->mNumVertices;
	subset.FaceStart = baseIndex / 3;
	subset.FaceCount = mesh->mNumFaces;

	for(UINT i = 0; i < mesh->mNumVertices; ++i)
	{
		M3DLoader::SkinnedVertex vertex = {};

		// Assimp vertex 속성을 M3DLoader::SkinnedVertex 레이아웃에 맞게 채운다.
		vertex.Pos = mesh->HasPositions() ? ToFloat3(mesh->mVertices[i]) : XMFLOAT3(0.0f, 0.0f, 0.0f);
		vertex.Normal = mesh->HasNormals() ? ToFloat3(mesh->mNormals[i]) : XMFLOAT3(0.0f, 1.0f, 0.0f);
		vertex.TexC = mesh->HasTextureCoords(0) ? ToFloat2(mesh->mTextureCoords[0][i]) : XMFLOAT2(0.0f, 0.0f);
		vertex.TangentU = mesh->HasTangentsAndBitangents() ? ToFloat3(mesh->mTangents[i]) : XMFLOAT3(1.0f, 0.0f, 0.0f);
		vertex.BoneWeights = XMFLOAT3(0.0f, 0.0f, 0.0f);

		for(int boneSlot = 0; boneSlot < 4; ++boneSlot)
			vertex.BoneIndices[boneSlot] = 0;

		mVertices->push_back(vertex);
		mVertexWeights.push_back({ 0.0f, 0.0f, 0.0f, 0.0f });
		mVertexBoneIndices.push_back({ 0, 0, 0, 0 });
	}

	for(UINT i = 0; i < mesh->mNumFaces; ++i)
	{
		const aiFace& face = mesh->mFaces[i];
		if(face.mNumIndices != 3)
		{
			mLastError = "A non-triangle face remained after Assimp triangulation.";
			return false;
		}

		// FBX 정점 수가 65536개를 넘을 수 있으므로 16비트가 아닌 32비트 인덱스를 저장한다.
		for(UINT j = 0; j < 3; ++j)
			mIndices->push_back((std::uint32_t)(baseVertex + face.mIndices[j]));
	}

	// Assimp의 material index는 FBX 내부 값이므로 렌더러에서는 subset마다 고유 이름을 붙여 충돌을 피한다.
	// Assimp의 material index는 FBX 내부 값이므로 렌더러에서는 subset마다 고유 material을 만든다.
	mSubsets->push_back(subset);
	mMats->push_back(ConvertMaterial(scene, mesh->mMaterialIndex, subsetIndex));

	ReadBones(mesh, baseVertex);
	return true;
}

void AssimpLoader::ReadBones(const aiMesh* mesh, UINT baseVertex)
{
	// mesh에 달린 bone offset matrix와 vertex weight를 읽는다.
	for(UINT boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
	{
		const aiBone* bone = mesh->mBones[boneIndex];
		const std::string boneName = bone->mName.C_Str();

		mBoneOffsetByName[boneName] = ToFloat4x4(bone->mOffsetMatrix);

		for(UINT weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex)
		{
			const aiVertexWeight& weight = bone->mWeights[weightIndex];
			PendingBoneWeight pending;
			pending.VertexIndex = baseVertex + weight.mVertexId;
			pending.BoneName = boneName;
			pending.Weight = weight.mWeight;
			mPendingWeights.push_back(pending);
		}
	}
}

bool AssimpLoader::ContainsWeightedBone(const aiNode* node) const
{
	if(node == nullptr)
		return false;

	// weight가 있는 bone이거나 그 자식 중 weight bone이 있으면 skeleton에 남긴다.
	const std::string nodeName = node->mName.C_Str();
	if(mBoneOffsetByName.find(nodeName) != mBoneOffsetByName.end())
		return true;

	for(UINT i = 0; i < node->mNumChildren; ++i)
	{
		if(ContainsWeightedBone(node->mChildren[i]))
			return true;
	}

	return false;
}

void AssimpLoader::BuildBoneHierarchy(const aiNode* node, int parentBoneIndex)
{
	if(node == nullptr)
		return;

	std::string nodeName = node->mName.C_Str();
	if(nodeName.empty())
		nodeName = "__AssimpRoot";

	// skinning에 필요한 노드만 bone으로 만들고, 필요 없는 장면 노드는 건너뛴다.
	const bool keepNode = ContainsWeightedBone(node);
	int currentParent = parentBoneIndex;

	if(keepNode)
	{
		aiVector3D scale;
		aiVector3D translation;
		aiQuaternion rotation;
		node->mTransformation.Decompose(scale, rotation, translation);

		// 애니메이션 키가 없는 bone은 이 bind pose transform을 기본값으로 사용한다.
		LocalTransform localTransform;
		localTransform.Translation = ToFloat3(translation);
		localTransform.Scale = ToFloat3(scale);
		localTransform.RotationQuat = ToFloat4(rotation);
		mBoneBindPoseByName[nodeName] = localTransform;

		currentParent = CreateBone(nodeName, parentBoneIndex);
	}

	for(UINT i = 0; i < node->mNumChildren; ++i)
		BuildBoneHierarchy(node->mChildren[i], currentParent);
}

void AssimpLoader::AddMissingBones()
{
	// offset matrix는 있는데 노드 계층에서 찾지 못한 bone은 root 아래에 붙여서 누락을 막는다.
	for(const auto& boneOffsetPair : mBoneOffsetByName)
	{
		if(mBoneNameToIndex.find(boneOffsetPair.first) != mBoneNameToIndex.end())
			continue;

		// 일부 FBX는 weight가 있는 bone이 노드 계층에서 이름으로 검색되지 않는다. 그런 경우 루트 아래에 붙인다.
		if(mBoneBindPoseByName.find(boneOffsetPair.first) == mBoneBindPoseByName.end())
			mBoneBindPoseByName[boneOffsetPair.first] = LocalTransform();

		CreateBone(boneOffsetPair.first, mBoneNames.empty() ? -1 : 0);
	}
}

void AssimpLoader::ApplyPendingBoneWeights()
{
	// FBX에서 읽은 bone 이름 기반 weight를 최종 bone index 기반 weight로 변환한다.
	for(const PendingBoneWeight& pending : mPendingWeights)
	{
		if(pending.VertexIndex >= mVertexWeights.size() || pending.Weight <= 0.0f)
			continue;

		const int boneIndex = FindBoneIndex(pending.BoneName);
		if(boneIndex < 0 || boneIndex > 255)
			continue;

		auto& weights = mVertexWeights[pending.VertexIndex];
		auto& indices = mVertexBoneIndices[pending.VertexIndex];

		int targetSlot = -1;
		for(int slot = 0; slot < 4; ++slot)
		{
			if(weights[slot] == 0.0f)
			{
				targetSlot = slot;
				break;
			}
		}

		if(targetSlot == -1)
		{
			// 한 정점에 4개보다 많은 weight가 있으면 가장 작은 weight를 더 큰 weight로 교체한다.
			targetSlot = 0;
			for(int slot = 1; slot < 4; ++slot)
			{
				if(weights[slot] < weights[targetSlot])
					targetSlot = slot;
			}

			if(pending.Weight <= weights[targetSlot])
				continue;
		}

		weights[targetSlot] = pending.Weight;
		indices[targetSlot] = (BYTE)boneIndex;
	}

	for(UINT vertexIndex = 0; vertexIndex < (UINT)mVertices->size(); ++vertexIndex)
	{
		auto& weights = mVertexWeights[vertexIndex];
		auto& indices = mVertexBoneIndices[vertexIndex];

		float weightSum = weights[0] + weights[1] + weights[2] + weights[3];
		if(weightSum <= 0.0f)
		{
			// weight가 없는 정점은 root bone에 100% 붙여 스키닝 계산이 안정적으로 되게 한다.
			weights = { 1.0f, 0.0f, 0.0f, 0.0f };
			indices = { 0, 0, 0, 0 };
			weightSum = 1.0f;
		}

		// 네 weight의 합이 1이 되도록 정규화한 뒤 셰이더 입력 형식에 맞춰 저장한다.
		for(int slot = 0; slot < 4; ++slot)
			weights[slot] /= weightSum;

		M3DLoader::SkinnedVertex& vertex = (*mVertices)[vertexIndex];
		vertex.BoneWeights = XMFLOAT3(weights[0], weights[1], weights[2]);

		for(int slot = 0; slot < 4; ++slot)
			vertex.BoneIndices[slot] = indices[slot];
	}
}

void AssimpLoader::ReadAnimations(const aiScene* scene)
{
	if(scene == nullptr || scene->mNumAnimations == 0 || mBoneNames.empty())
		return;

	// FBX 안에 들어 있는 animation clip들을 전부 SkinnedData 형식으로 변환한다.
	for(UINT animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
	{
		const aiAnimation* animation = scene->mAnimations[animationIndex];
		// FBX key 시간은 tick 단위라서 seconds로 변환해야 재생 속도가 맞는다.
		const double ticksPerSecond = animation->mTicksPerSecond != 0.0 ? animation->mTicksPerSecond : 25.0;
		const float durationSeconds = (float)ToSeconds(animation->mDuration, ticksPerSecond);

		std::string clipName = animation->mName.length > 0 ? animation->mName.C_Str() : "";
		if(clipName.empty())
			clipName = "Anim" + std::to_string(animationIndex);

		clipName = MakeSafeName(clipName, "Anim" + std::to_string(animationIndex));

		AnimationClip clip;
		clip.BoneAnimations.resize(mBoneNames.size());

		// channel이 없는 bone도 bind pose로 움직이지 않는 keyframe을 넣어 전체 bone 개수를 맞춘다.
		for(UINT boneIndex = 0; boneIndex < (UINT)mBoneNames.size(); ++boneIndex)
			clip.BoneAnimations[boneIndex] = BuildStaticBoneAnimation(mBoneNames[boneIndex], durationSeconds);

		for(UINT channelIndex = 0; channelIndex < animation->mNumChannels; ++channelIndex)
		{
			// channel 이름이 skeleton의 bone 이름과 맞을 때만 해당 bone animation으로 사용한다.
			const aiNodeAnim* channel = animation->mChannels[channelIndex];
			const std::string boneName = channel->mNodeName.C_Str();
			const int boneIndex = FindBoneIndex(boneName);
			if(boneIndex < 0)
				continue;

			const LocalTransform bindPose = GetBindPoseTransform(boneName);
			std::vector<double> times;
			AddUniqueTime(times, 0.0);
			AddUniqueTime(times, durationSeconds);

			// position/rotation/scale key 시간이 따로 올 수 있어서 모든 시간을 합친 뒤 샘플링한다.
			for(UINT i = 0; i < channel->mNumPositionKeys; ++i)
				AddUniqueTime(times, ToSeconds(channel->mPositionKeys[i].mTime, ticksPerSecond));

			for(UINT i = 0; i < channel->mNumRotationKeys; ++i)
				AddUniqueTime(times, ToSeconds(channel->mRotationKeys[i].mTime, ticksPerSecond));

			for(UINT i = 0; i < channel->mNumScalingKeys; ++i)
				AddUniqueTime(times, ToSeconds(channel->mScalingKeys[i].mTime, ticksPerSecond));

			SortAndUniqueTimes(times);

			BoneAnimation boneAnimation;
			for(double time : times)
			{
				// 해당 시간의 position/scale/rotation을 각각 보간해서 하나의 keyframe으로 만든다.
				Keyframe keyframe;
				keyframe.TimePos = (float)time;
				keyframe.Translation = SampleVectorKey(
					channel->mPositionKeys,
					channel->mNumPositionKeys,
					time,
					ticksPerSecond,
					bindPose.Translation);
				keyframe.Scale = SampleVectorKey(
					channel->mScalingKeys,
					channel->mNumScalingKeys,
					time,
					ticksPerSecond,
					bindPose.Scale);
				keyframe.RotationQuat = SampleRotationKey(
					channel->mRotationKeys,
					channel->mNumRotationKeys,
					time,
					ticksPerSecond,
					bindPose.RotationQuat);

				boneAnimation.Keyframes.push_back(keyframe);
			}

			if(!boneAnimation.Keyframes.empty())
				clip.BoneAnimations[boneIndex] = boneAnimation;
		}

		mAnimations[clipName] = clip;
	}
}

M3DLoader::M3dMaterial AssimpLoader::ConvertMaterial(const aiScene* scene, UINT materialIndex, UINT subsetIndex) const
{
	// 기본값은 흰색 diffuse와 기본 normal map으로 둬서 텍스처가 없어도 모델이 그려지게 한다.
	M3DLoader::M3dMaterial material;
	material.Name = "skinned_" + std::to_string(subsetIndex);
	material.MaterialTypeName = "Skinned";
	material.DiffuseMapName = "white1x1.dds";
	material.NormalMapName = "default_nmap.dds";

	if(scene == nullptr || materialIndex >= scene->mNumMaterials)
		return material;

	const aiMaterial* sourceMaterial = scene->mMaterials[materialIndex];

	aiString sourceName;
	if(sourceMaterial->Get(AI_MATKEY_NAME, sourceName) == AI_SUCCESS)
	{
		const std::string safeMaterialName = MakeSafeName(sourceName.C_Str(), "material");
		material.Name = "skinned_" + std::to_string(subsetIndex) + "_" + safeMaterialName;
	}

	aiColor3D diffuse(1.0f, 1.0f, 1.0f);
	if(sourceMaterial->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
		material.DiffuseAlbedo = XMFLOAT4(diffuse.r, diffuse.g, diffuse.b, 1.0f);

	float opacity = 1.0f;
	if(sourceMaterial->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS)
	{
		material.DiffuseAlbedo.w = opacity;
		material.AlphaClip = opacity < 1.0f;
	}

	float shininess = 0.0f;
	if(sourceMaterial->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f)
		material.Roughness = MathHelper::Clamp(1.0f - shininess / 128.0f, 0.05f, 1.0f);

	aiString texturePath;

	if (sourceMaterial->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS)
	{
		const aiTexture* embeddedTex = scene->GetEmbeddedTexture(texturePath.C_Str());

		if (embeddedTex)
		{
			// FBX 안에 들어있는 텍스처
			OutputDebugStringA("Embedded texture found\n");
		}
		else
		{
			// 외부 png 파일
			OutputDebugStringA("External texture path: ");
			OutputDebugStringA(texturePath.C_Str());
			OutputDebugStringA("\n");
		}
	}

	if(sourceMaterial->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS)
	{
		const aiTexture* embeddedTex = scene->GetEmbeddedTexture(texturePath.C_Str());

		if(embeddedTex)
		{
			const std::filesystem::path outputFolder =
				std::filesystem::path(mModelDirectory) / "_EmbeddedTextures";
			const std::string savedTexturePath = SaveEmbeddedTextureToFile(
				scene,
				texturePath,
				outputFolder,
				material.Name + "_diffuse");

			if(!savedTexturePath.empty())
				material.DiffuseMapName = savedTexturePath;
		}
		else
		{
			const std::string resolvedTexturePath =
				ResolveExternalTexturePath(texturePath.C_Str(), mModelDirectory);

			if(!resolvedTexturePath.empty())
				material.DiffuseMapName = resolvedTexturePath;
		}
	}

	if(sourceMaterial->GetTexture(aiTextureType_NORMALS, 0, &texturePath) == AI_SUCCESS ||
		sourceMaterial->GetTexture(aiTextureType_HEIGHT, 0, &texturePath) == AI_SUCCESS)
	{
		const std::string textureName = ExtractFilename(texturePath.C_Str());
		// normal map도 DDS일 때만 샘플의 텍스처 로더로 연결한다.
		if(IsDdsTexture(textureName))
			material.NormalMapName = textureName;
	}

	return material;
}

int AssimpLoader::CreateBone(const std::string& boneName, int parentBoneIndex)
{
	// 이미 만든 bone이면 같은 index를 재사용해서 hierarchy와 weight index가 어긋나지 않게 한다.
	auto found = mBoneNameToIndex.find(boneName);
	if(found != mBoneNameToIndex.end())
		return found->second;

	const int boneIndex = (int)mBoneNames.size();
	mBoneNameToIndex[boneName] = boneIndex;
	mBoneNames.push_back(boneName);

	auto offsetFound = mBoneOffsetByName.find(boneName);
	// FBX offset matrix가 없으면 identity를 넣어 정적/root bone도 안전하게 처리한다.
	mBoneOffsets.push_back(offsetFound != mBoneOffsetByName.end() ? offsetFound->second : MathHelper::Identity4x4());

	if(boneIndex == 0)
		mBoneHierarchy.push_back(0);
	else
		mBoneHierarchy.push_back(parentBoneIndex >= 0 ? parentBoneIndex : 0);

	return boneIndex;
}

int AssimpLoader::FindBoneIndex(const std::string& boneName) const
{
	// animation channel과 vertex weight가 참조하는 bone 이름을 최종 index로 바꿀 때 사용한다.
	auto found = mBoneNameToIndex.find(boneName);
	return found == mBoneNameToIndex.end() ? -1 : found->second;
}

AssimpLoader::LocalTransform AssimpLoader::GetBindPoseTransform(const std::string& boneName) const
{
	// animation key가 없는 bone은 저장된 bind pose를 기본 transform으로 사용한다.
	auto found = mBoneBindPoseByName.find(boneName);
	return found == mBoneBindPoseByName.end() ? LocalTransform() : found->second;
}

BoneAnimation AssimpLoader::BuildStaticBoneAnimation(const std::string& boneName, float endTime) const
{
	// animation이 없거나 channel이 빠진 bone을 위해 bind pose만 가진 정적 animation을 만든다.
	const LocalTransform bindPose = GetBindPoseTransform(boneName);

	Keyframe startFrame;
	startFrame.TimePos = 0.0f;
	startFrame.Translation = bindPose.Translation;
	startFrame.Scale = bindPose.Scale;
	startFrame.RotationQuat = bindPose.RotationQuat;

	Keyframe endFrame = startFrame;
	endFrame.TimePos = endTime > 0.0f ? endTime : 1.0f / 30.0f;

	BoneAnimation animation;
	animation.Keyframes.push_back(startFrame);
	animation.Keyframes.push_back(endFrame);

	return animation;
}

XMFLOAT2 AssimpLoader::ToFloat2(const aiVector3D& value)
{
	return XMFLOAT2(value.x, value.y);
}

XMFLOAT3 AssimpLoader::ToFloat3(const aiVector3D& value)
{
	return XMFLOAT3(value.x, value.y, value.z);
}

XMFLOAT4 AssimpLoader::ToFloat4(const aiQuaternion& value)
{
	return XMFLOAT4(value.x, value.y, value.z, value.w);
}

XMFLOAT4X4 AssimpLoader::ToFloat4x4(const aiMatrix4x4& value)
{
	// Assimp matrix는 DirectX 샘플의 row-vector 행렬 규칙과 방향이 달라 전치해서 저장한다.
	// 이 처리가 없으면 bone offset의 이동 성분이 잘못 들어가 모델 정점이 크게 찢어진다.
	// Assimp의 aiMatrix4x4는 이 DirectX 샘플의 row-vector 행렬 규칙과 반대라서 전치해서 넣는다.
	// 특히 bone offset의 이동 성분이 4열이 아니라 4행에 있어야 스키닝이 찢어지지 않는다.
	return XMFLOAT4X4(
		value.a1, value.b1, value.c1, value.d1,
		value.a2, value.b2, value.c2, value.d2,
		value.a3, value.b3, value.c3, value.d3,
		value.a4, value.b4, value.c4, value.d4);
}

std::string AssimpLoader::ExtractFilename(const std::string& path)
{
	// FBX material texture path에서 파일명만 꺼내 ../../Textures 폴더에서 찾게 한다.
	const size_t slash = path.find_last_of("/\\");
	if(slash == std::string::npos)
		return path;

	return path.substr(slash + 1);
}

bool AssimpLoader::IsDdsTexture(const std::string& filename)
{
	// 이 샘플의 텍스처 로더는 DDS만 바로 읽으므로 확장자를 검사한다.
	const size_t dot = filename.find_last_of('.');
	if(dot == std::string::npos)
		return false;

	std::string extension = filename.substr(dot);
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char ch)
		{
			return (char)std::tolower(ch);
		});

	return extension == ".dds";
}

XMFLOAT3 AssimpLoader::SampleVectorKey(
	const aiVectorKey* keys,
	UINT keyCount,
	double time,
	double ticksPerSecond,
	const XMFLOAT3& defaultValue) const
{
	if(keys == nullptr || keyCount == 0)
		return defaultValue;

	// key 시간은 tick으로 들어오므로 비교와 보간은 모두 seconds 기준으로 한다.
	const double firstTime = ToSeconds(keys[0].mTime, ticksPerSecond);
	const double lastTime = ToSeconds(keys[keyCount - 1].mTime, ticksPerSecond);

	if(time <= firstTime)
		return ToFloat3(keys[0].mValue);

	if(time >= lastTime)
		return ToFloat3(keys[keyCount - 1].mValue);

	for(UINT i = 0; i < keyCount - 1; ++i)
	{
		const double keyTime0 = ToSeconds(keys[i].mTime, ticksPerSecond);
		const double keyTime1 = ToSeconds(keys[i + 1].mTime, ticksPerSecond);

		if(time >= keyTime0 && time <= keyTime1)
		{
			// position/scale key는 선형 보간한다.
			const double range = keyTime1 - keyTime0;
			const float lerpAmount = range > 0.0 ? (float)((time - keyTime0) / range) : 0.0f;

			const XMFLOAT3 start = ToFloat3(keys[i].mValue);
			const XMFLOAT3 end = ToFloat3(keys[i + 1].mValue);

			return XMFLOAT3(
				start.x + (end.x - start.x) * lerpAmount,
				start.y + (end.y - start.y) * lerpAmount,
				start.z + (end.z - start.z) * lerpAmount);
		}
	}

	return ToFloat3(keys[keyCount - 1].mValue);
}

XMFLOAT4 AssimpLoader::SampleRotationKey(
	const aiQuatKey* keys,
	UINT keyCount,
	double time,
	double ticksPerSecond,
	const XMFLOAT4& defaultValue) const
{
	if(keys == nullptr || keyCount == 0)
		return defaultValue;

	// 회전 key도 tick 시간을 seconds로 변환한 뒤 보간 구간을 찾는다.
	const double firstTime = ToSeconds(keys[0].mTime, ticksPerSecond);
	const double lastTime = ToSeconds(keys[keyCount - 1].mTime, ticksPerSecond);

	if(time <= firstTime)
		return ToFloat4(keys[0].mValue);

	if(time >= lastTime)
		return ToFloat4(keys[keyCount - 1].mValue);

	for(UINT i = 0; i < keyCount - 1; ++i)
	{
		const double keyTime0 = ToSeconds(keys[i].mTime, ticksPerSecond);
		const double keyTime1 = ToSeconds(keys[i + 1].mTime, ticksPerSecond);

		if(time >= keyTime0 && time <= keyTime1)
		{
			// rotation key는 quaternion slerp로 보간해야 회전이 자연스럽다.
			const double range = keyTime1 - keyTime0;
			const float lerpAmount = range > 0.0 ? (float)((time - keyTime0) / range) : 0.0f;

			const XMFLOAT4 start = ToFloat4(keys[i].mValue);
			const XMFLOAT4 end = ToFloat4(keys[i + 1].mValue);
			const XMVECTOR q0 = XMLoadFloat4(&start);
			const XMVECTOR q1 = XMLoadFloat4(&end);
			const XMVECTOR q = XMQuaternionNormalize(XMQuaternionSlerp(q0, q1, lerpAmount));

			XMFLOAT4 result;
			XMStoreFloat4(&result, q);
			return result;
		}
	}

	return ToFloat4(keys[keyCount - 1].mValue);
}

double AssimpLoader::ToSeconds(double ticks, double ticksPerSecond) const
{
	// FBX에 ticksPerSecond가 없으면 Assimp 관례값인 25fps 기준으로 환산한다.
	return ticks / (ticksPerSecond > 0.0 ? ticksPerSecond : 25.0);
}
