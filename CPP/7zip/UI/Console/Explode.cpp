#include "StdAfx.h"
#include <sstream>

#include "Explode.h"
#include "Windows/Error.h"
#include "../../Archive/IArchive.h"
#include "../Common/OpenArchive.h"
#include "OpenCallbackConsole.h"
#include "../../Archive/7z/7zItem.h"
#include "../../Archive/7z/7zOut.h"
#include "../../Archive/7z/7zHandler.h"
#include "../../Common/FileStreams.h"
#include "../../Common/LimitedStreams.h"
#include "../../Compress/CopyCoder.h"
#include "../../../Windows/FileDir.h"

#include <assert.h>

using namespace NWindows;

#define SHOW_ERROR(x) g_StdOut << endl << "Error: " << archivePath << ": " \
	<< x << endl; numErrors++; 

static HRESULT WriteRange(IInStream *inStream, ISequentialOutStream *outStream,
	UInt64 position, UInt64 size, ICompressProgressInfo *progress)
{
	RINOK(inStream->Seek(position, STREAM_SEEK_SET, 0));
	CLimitedSequentialInStream *streamSpec = new CLimitedSequentialInStream;
	CMyComPtr<CLimitedSequentialInStream> inStreamLimited(streamSpec);
	streamSpec->SetStream(inStream);
	streamSpec->Init(size);

	NCompress::CCopyCoder *copyCoderSpec = new NCompress::CCopyCoder;
	CMyComPtr<ICompressCoder> copyCoder = copyCoderSpec;
	RINOK(copyCoder->Code(inStreamLimited, outStream, NULL, NULL, progress));
	return (copyCoderSpec->TotalSize == size ? S_OK : E_FAIL);
}

// Strip the file name from the full path, keeping the trailing /
void StripFile(UString& path) 
{
	// remove file name
	int last = path.ReverseFind(L'/');
	if (last != -1) {
		int namelen = path.Length() - last;
		path.Delete(last + 1, namelen - 1);
	} else {// file not directory
		path.Empty();
	}
}

// Get the file name from a path
UString GetFileFromPath(const UString& path)
{
	UString file;
	int last = path.ReverseFind(L'/');
	if (last != -1) {
		int namelen = path.Length() - last;
		file = path.Right(namelen - 1);	
	} else file = path;
	return file;
}

// Make sure the path is terminated with only a single /
void FixPathFormat(UString& path)
{
	while (path.Back() == L'/') path.DeleteBack();
	path += L'/';
}


// Create a new 7z archive for each folder contained in the archive to be
// exploded.
HRESULT ExplodeArchives(CCodecs *codecs, const CIntVector &formatIndices,
	bool stdInMode,
	UStringVector &arcPaths, UStringVector &arcPathsFull,
	UString& outputPath,
	UInt64 &numErrors)
{
	int numArcs = arcPaths.Size();
	for (int i = 0; i < numArcs; i++)
	{
		UString archivePath = arcPaths[i];
		
		/*UString outputPath = arcPaths[i];
		outputPath.Replace(L'\\', L'/'); // linux and windows consistent
		const UString archiveName = StripFile(outputPath);
		outputPath.Empty();*/
		archivePath.Replace(L'\\', L'/'); // linux, windows and archive consistent
		outputPath.Replace(L'\\', L'/'); 
		FixPathFormat(outputPath);
		const UString archiveName = GetFileFromPath(archivePath);

		g_StdOut << "Outputting into : " << outputPath << endl;

		UInt64 arcPackSize = 0;
		if (!stdInMode)
		{
			NFile::NFind::CFileInfoW fi;
			if (!fi.Find(archivePath) || fi.IsDir())
			{
				SHOW_ERROR("is not a file.");
				continue;
			}
			arcPackSize = fi.Size;
		}

		g_StdOut << endl << "Exploding : " << archivePath << endl << endl;

		CArchiveLink archiveLink;

		COpenCallbackConsole openCallback;
		openCallback.OutStream = &g_StdOut;

#ifndef _NO_CRYPTO
		openCallback.PasswordIsDefined = false;
#endif

		HRESULT result = archiveLink.Open2(codecs, formatIndices, stdInMode, NULL, archivePath, &openCallback);
	
		if (result != S_OK)
		{
			if (result == E_ABORT)
				return result;
			g_StdOut << endl << "Error: " << archivePath << ": ";
			if (result == S_FALSE)
				g_StdOut << "Can not open file as archive";
			else if (result == E_OUTOFMEMORY)
				g_StdOut << "Can't allocate required memory";
			else
				g_StdOut << NError::MyFormatMessage(result);
			g_StdOut << endl;
			numErrors++;
			continue;
		}

		// remove other files names if multi-volume
		if (!stdInMode) {
			for (int v = 0; v < archiveLink.VolumePaths.Size(); v++)
			{
				int index = arcPathsFull.FindInSorted(archiveLink.VolumePaths[v]);
				if (index >= 0 && index > i)
				{
					arcPaths.Delete(index);
					arcPathsFull.Delete(index);
					numArcs = arcPaths.Size();
				}
			}
		}

		// don't support multi volume because i have to reopen the stream
		if (archiveLink.VolumePaths.Size() != 1) {
			SHOW_ERROR("Exploding multi-volume archives isn't supported.");
			continue;
		}

		bool szArchive = true;
		for (int x = 0; x < archiveLink.Arcs.Size(); x++)
		{
			const UString szName = L"7z";
			const CArc &arc = archiveLink.Arcs[x];
			if (codecs->Formats[arc.FormatIndex].Name != szName) {
				szArchive = false;
				break;
			}
		}

		if (!szArchive) {
			SHOW_ERROR("Only 7z archives can be exploded.");
			continue;
		}

		// should probably change this and add explode to IInArchive
		// and implement there, but then I need to add dummy handlers
		// for other formats because only 7z is supported
		using namespace NArchive::N7z;
		IInArchive* inArc = archiveLink.GetArchive();
		CHandler* szHandler = (CHandler*)inArc;

		// not a fan of having to reopen the file..
		CInFileStream* _inStream = new CInFileStream;
		CMyComPtr<CInFileStream> inStream(_inStream);
		if (!inStream->Open(archivePath)) {
			SHOW_ERROR("Cannot be opened for reading.");
			continue;
		}	

		// Explode the archive into each folder
		CObjectVector<CArchiveDatabase> exploded;
		CRecordVector<UInt64> folderSizes, folderPositions;
		szHandler->Explode(exploded, folderSizes, folderPositions);
	
		if (exploded.Size() == 0) {
			SHOW_ERROR("Empty archive!");
			continue;
		}
		
		// Save each folder as a new 7z archive
		for (int x = 0; x < exploded.Size(); x++) {
			UInt64 folderLen = folderSizes[x];
			UInt64 folderStartPackPos = folderPositions[x];
			
			UString relativeFilePath; // relative to archive
			UString fileName;

			// each exploded archive will only have a single folder.
			if (exploded[x].Files.Size() > 0) {
				relativeFilePath = exploded[x].Files[0].Name;
				if (!exploded[x].Files[0].IsDir) {
					fileName = GetFileFromPath(relativeFilePath);
					StripFile(relativeFilePath);
				}
			}

			//g_StdOut << "Relative path " << relativeFilePath << endl;
			//g_StdOut << "Archive " << archivePath << endl;

			UString folderOutPath = outputPath + relativeFilePath;
			if (relativeFilePath.Length() != 0) {
				bool b = NWindows::NFile::NDirectory::CreateComplexDirectory(folderOutPath);
				if (!b) g_StdOut << "Couldn't create directory " << folderOutPath << endl;
				//relativeFilePath.Insert(folderOutPath.Length(), L'/');
			}

			std::wstringstream sstream;
			sstream << folderOutPath.GetBuffer();
			
			if (exploded[x].Files.Size() == 1) // can use file names
				sstream << fileName.GetBuffer();
			else // use folder as name 
				sstream << archiveName.GetBuffer() << L"_folder_" << x;
			sstream << ".7z";
			
			g_StdOut << "Saving as '" << sstream.str().c_str() << "'" << endl;

			COutFileStream* _outstream = new COutFileStream;
			CMyComPtr<COutFileStream> outstream(_outstream);
			outstream->Create(sstream.str().c_str(), true);

			COutArchive out;
			out.Create(outstream, false);
			out.SkipPrefixArchiveHeader();
	
			// write actual data
			RINOK(WriteRange(inStream, out.SeqStream, 
			folderStartPackPos, folderLen, NULL));

			CCompressionMethodMode method, headerMethod;
			szHandler->SetCompressionMethod(method, headerMethod);

			CHeaderOptions headerOptions;
			headerOptions.CompressMainHeader = true;

			out.WriteDatabase(exploded[x], &headerMethod, headerOptions);
			out.Close();
		}		

		archiveLink.Close(); // not needed but oh well
	}

	return S_OK;
}