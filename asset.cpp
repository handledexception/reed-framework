#include "framework.h"
#include "miniz.h"

#include <sys/types.h>
#include <sys/stat.h>

namespace Framework
{
	// AssetPack implementation

	bool AssetPack::LookupFile(const char * path, void ** ppDataOut, int * pSizeOut)
	{
		ASSERT_ERR(path);

		// !!!UNDONE: assert that path is in a normal form (lowercase, forward slashes, etc.)?

		auto iter = m_directory.find(path);
		if (iter == m_directory.end())
			return false;

		int iFile = iter->second;
		const FileInfo & fileinfo = m_files[iFile];

		if (ppDataOut)
			*ppDataOut = &m_data[fileinfo.m_offset];
		if (pSizeOut)
			*pSizeOut = fileinfo.m_size;

		return true;
	}

	bool AssetPack::LookupFile(const char * path, const char * suffix, void ** ppDataOut, int * pSizeOut)
	{
		ASSERT_ERR(path);
		ASSERT_ERR(suffix);

		// !!!UNDONE: assert that path is in a normal form (lowercase, forward slashes, etc.)?

		std::string fullPath = path;
		fullPath += suffix;
		auto iter = m_directory.find(fullPath);
		if (iter == m_directory.end())
			return false;

		int iFile = iter->second;
		const FileInfo & fileinfo = m_files[iFile];

		if (ppDataOut)
			*ppDataOut = &m_data[fileinfo.m_offset];
		if (pSizeOut)
			*pSizeOut = fileinfo.m_size;

		return true;
	}

	void AssetPack::Reset()
	{
		m_data.clear();
		m_files.clear();
		m_directory.clear();
		m_path.clear();
	}


	
	// Asset loading & compilation

	// Prototype individual compilation functions for different asset types

	bool CompileOBJMeshAsset(
		const AssetCompileInfo * pACI,
		mz_zip_archive * pZipOut);
	bool CompileTextureRawAsset(
		const AssetCompileInfo * pACI,
		mz_zip_archive * pZipOut);
	bool CompileTextureWithMipsAsset(
		const AssetCompileInfo * pACI,
		mz_zip_archive * pZipOut);

	typedef bool (*AssetCompileFunc)(const AssetCompileInfo *, mz_zip_archive *);
	static const AssetCompileFunc s_assetCompileFuncs[] =
	{
		&CompileOBJMeshAsset,				// ACK_OBJMesh
		&CompileTextureRawAsset,			// ACK_TextureRaw
		&CompileTextureWithMipsAsset,		// ACK_TextureWithMips
	};
	cassert(dim(s_assetCompileFuncs) == ACK_Count);

	// Load an asset pack file, checking that all its assets are present and up to date,
	// and compiling any that aren't.
	bool LoadAssetPackOrCompileIfOutOfDate(
		const char * packPath,
		const AssetCompileInfo * assets,
		int numAssets,
		AssetPack * pPackOut)
	{
		ASSERT_ERR(packPath);
		ASSERT_ERR(assets);
		ASSERT_ERR(numAssets > 0);
		ASSERT_ERR(pPackOut);

#if LATER
		// Does the asset pack already exist?
		struct _stat packStat;
		if (_stat(packPath, &packStat) == 0)
		{
			// Ensure all assets are up to date
			if (!UpdateAssetPack(packPath, assets, numAssets))
				return false;
		}
		else
#endif
		{
			// The pack file doesn't exist.  Compile it from the source assets.
			if (!CompileFullAssetPack(packPath, assets, numAssets))
				return false;
		}

		// It ought to exist and be up-to-date now, so load it
		return LoadAssetPack(packPath, pPackOut);
	}

	// Just load an asset pack file, THAT'S ALL.
	bool LoadAssetPack(
		const char * packPath,
		AssetPack * pPackOut)
	{
		ASSERT_ERR(packPath);
		ASSERT_ERR(pPackOut);
		
		// Load the archive directory
		mz_zip_archive zip = {};
		if (!mz_zip_reader_init_file(&zip, packPath, 0))
		{
			WARN("Couldn't load asset pack %s", packPath);
			return false;
		}

		pPackOut->m_path = packPath;

		int numFiles = int(mz_zip_reader_get_num_files(&zip));
		pPackOut->m_files.resize(numFiles);
		pPackOut->m_directory.clear();
		pPackOut->m_directory.reserve(numFiles);

		// Run through all the files, build the file list and directory and sum up their sizes
		int bytesTotal = 0;
		for (int i = 0; i < numFiles; ++i)
		{
			mz_zip_archive_file_stat fileStat;
			if (!mz_zip_reader_file_stat(&zip, i, &fileStat))
			{
				WARN("Couldn't read directory entry %d of %d from asset pack %s", i, numFiles, packPath);
				mz_zip_reader_end(&zip);
				return false;
			}

			AssetPack::FileInfo * pFileInfo = &pPackOut->m_files[i];
			pFileInfo->m_path = fileStat.m_filename;
			pFileInfo->m_offset = bytesTotal;
			pFileInfo->m_size = int(fileStat.m_uncomp_size);

			pPackOut->m_directory.insert(std::make_pair(pFileInfo->m_path, i));

			bytesTotal += int(fileStat.m_uncomp_size);
		}

		// Allocate memory to store the decompressed data
		pPackOut->m_data.resize(bytesTotal);

		// Decompress all the files
		for (int i = 0; i < numFiles; ++i)
		{
			AssetPack::FileInfo * pFileInfo = &pPackOut->m_files[i];

			// Skip zero size files (trailing ones will cause an std::vector assert)
			if (pFileInfo->m_size == 0)
				continue;

			if (!mz_zip_reader_extract_to_mem(
					&zip, i,
					&pPackOut->m_data[pFileInfo->m_offset],
					pFileInfo->m_size, 0))
			{
				WARN("Couldn't extract file %s (index %d of %d) from asset pack %s", pFileInfo->m_path.c_str(), i, numFiles, packPath);
				mz_zip_reader_end(&zip);
				return false;
			}
		}

		mz_zip_reader_end(&zip);

		LOG("Loaded asset pack %s - %d files, %d total bytes", packPath, numFiles, bytesTotal);
		return true;
	}

	// Compile an entire asset pack from scratch.
	bool CompileFullAssetPack(
		const char * packPath,
		const AssetCompileInfo * assets,
		int numAssets)
	{
		ASSERT_ERR(packPath);
		ASSERT_ERR(assets);
		ASSERT_ERR(numAssets > 0);

		mz_zip_archive zip = {};
		if (!mz_zip_writer_init_file(&zip, packPath, 0))
		{
			WARN("Couldn't open %s for writing", packPath);
			return false;
		}

		// !!!UNDONE: not nicely generating entries in the .zip for directories in the internal paths.
		// Doesn't seem to matter as other .zip viewers handle it, but maybe we should do that anyway?

		int numErrors = 0;
		for (int iAsset = 0; iAsset < numAssets; ++iAsset)
		{
			const AssetCompileInfo * pACI = &assets[iAsset];
			ACK ack = pACI->m_ack;

			ASSERT_ERR(ack >= 0 && ack < ACK_Count);
			if (!s_assetCompileFuncs[ack](pACI, &zip))
			{
				WARN("Couldn't compile asset %s", pACI->m_pathSrc);
				++numErrors;
			}
		}

		if (numErrors > 0)
		{
			WARN("Failed to compile %d of %d assets", numErrors, numAssets);
		}

		if (!mz_zip_writer_finalize_archive(&zip))
		{
			WARN("Couldn't finalize archive %s", packPath);
			mz_zip_writer_end(&zip);
			return false;
		}

		mz_zip_writer_end(&zip);

		return (numErrors == 0);
	}

	// Update an asset pack by compiling the assets that are out of date or missing from it.
	bool UpdateAssetPack(
		const char * packPath,
		const AssetCompileInfo * assets,
		int numAssets)
	{
		ASSERT_ERR(packPath);
		ASSERT_ERR(assets);
		ASSERT_ERR(numAssets > 0);
		
		// Load the archive directory
		mz_zip_archive zip = {};
		if (!mz_zip_reader_init_file(&zip, packPath, 0))
		{
			WARN("Couldn't load asset pack %s", packPath);
			return false;
		}

		// !!!UNDONE

		mz_zip_reader_end(&zip);
		return true;
	}
}
