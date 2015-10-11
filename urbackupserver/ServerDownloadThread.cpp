
/*************************************************************************
*    UrBackup - Client/Server backup system
*    Copyright (C) 2011-2015 Martin Raiber
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU Affero General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include <algorithm>

#include "ServerDownloadThread.h"
#include "../Interface/Server.h"
#include "server_log.h"
#include "ClientMain.h"
#include "../stringtools.h"
#include "../common/data.h"
#include "../urbackupcommon/file_metadata.h"
#include "server_settings.h"
#include "server_cleanup.h"
#include "FileBackup.h"

namespace
{
	const unsigned int shadow_copy_timeout=30*60*1000;
	const size_t max_queue_size = 500;
	const size_t queue_items_full = 1;
	const size_t queue_items_chunked = 4;
}

ServerDownloadThread::ServerDownloadThread( FileClient& fc, FileClientChunked* fc_chunked, const std::wstring& backuppath, const std::wstring& backuppath_hashes, const std::wstring& last_backuppath, const std::wstring& last_backuppath_complete, bool hashed_transfer, bool save_incomplete_file, int clientid,
	const std::wstring& clientname, bool use_tmpfiles, const std::wstring& tmpfile_path, const std::string& server_token, bool use_reflink, int backupid, bool r_incremental, IPipe* hashpipe_prepare, ClientMain* client_main,
	int filesrv_protocol_version, int incremental_num, logid_t logid)
	: fc(fc), fc_chunked(fc_chunked), backuppath(backuppath), backuppath_hashes(backuppath_hashes), 
	last_backuppath(last_backuppath), last_backuppath_complete(last_backuppath_complete), hashed_transfer(hashed_transfer), save_incomplete_file(save_incomplete_file), clientid(clientid),
	clientname(clientname),
	use_tmpfiles(use_tmpfiles), tmpfile_path(tmpfile_path), server_token(server_token), use_reflink(use_reflink), backupid(backupid), r_incremental(r_incremental), hashpipe_prepare(hashpipe_prepare), max_ok_id(0),
	is_offline(false), client_main(client_main), filesrv_protocol_version(filesrv_protocol_version), skipping(false), queue_size(0),
	all_downloads_ok(true), incremental_num(incremental_num), logid(logid)
{
	mutex = Server->createMutex();
	cond = Server->createCondition();
}

ServerDownloadThread::~ServerDownloadThread()
{
	Server->destroy(mutex);
	Server->destroy(cond);
}

void ServerDownloadThread::operator()( void )
{
	if(fc_chunked!=NULL && filesrv_protocol_version>2)
	{
		fc_chunked->setQueueCallback(this);
	}

	while(true)
	{
		SQueueItem curr;
		{
			IScopedLock lock(mutex);
			while(dl_queue.empty())
			{
				cond->wait(&lock);
			}
			curr = dl_queue.front();
			dl_queue.pop_front();

			if(curr.action == EQueueAction_Fileclient)
			{
				if(curr.fileclient == EFileClient_Full)
				{
					queue_size-=queue_items_full;
				}
				else if(curr.fileclient== EFileClient_Chunked)
				{
					queue_size-=queue_items_chunked;
				}
			}			
		}

		if(curr.action==EQueueAction_Quit)
		{
			break;
		}
		else if(curr.action==EQueueAction_Skip)
		{
			skipping = true;
			continue;
		}

		if(is_offline || skipping)
		{
			if(curr.fileclient== EFileClient_Chunked)
			{
				ServerLogger::Log(logid, L"Copying incomplete file \"" + curr.fn+ L"\"", LL_DEBUG);								
				bool full_dl = false;
				
				if(!curr.patch_dl_files.prepared)
				{
					curr.patch_dl_files = preparePatchDownloadFiles(curr, full_dl);
				}				

				if(!full_dl && curr.patch_dl_files.prepared 
				    && !curr.patch_dl_files.prepare_error && curr.patch_dl_files.orig_file!=NULL)
				{
					if(link_or_copy_file(curr))
					{
						download_partial_ids.add(curr.id);
						max_ok_id = (std::max)(max_ok_id, curr.id);
					}
					else
		    		{
						ServerLogger::Log(logid, L"Copying incomplete file \""+curr.fn+L"\" failed", LL_WARNING);
						download_nok_ids.add(curr.id);					
						
						IScopedLock lock(mutex);
						all_downloads_ok=false;
					}
				
					continue;
				}
			}
				
		    download_nok_ids.add(curr.id);

			{
				IScopedLock lock(mutex);
				all_downloads_ok=false;
			}

			if(curr.patch_dl_files.prepared)
			{
				delete curr.patch_dl_files.orig_file;
				ScopedDeleteFile del_1(curr.patch_dl_files.patchfile);
				ScopedDeleteFile del_2(curr.patch_dl_files.hashoutput);
				if(curr.patch_dl_files.delete_chunkhashes)
				{
					ScopedDeleteFile del_3(curr.patch_dl_files.chunkhashes);
				}
				else
				{
					delete curr.patch_dl_files.chunkhashes;
				}
			}

			continue;
		}

		if(curr.action==EQueueAction_StartShadowcopy)
		{
			start_shadowcopy(Server->ConvertToUTF8(curr.fn));
			continue;
		}
		else if(curr.action==EQueueAction_StopShadowcopy)
		{
			stop_shadowcopy(Server->ConvertToUTF8(curr.fn));
			continue;
		}		

		bool ret = true;

		if(curr.fileclient == EFileClient_Full)
		{
			ret = load_file(curr);
		}
		else if(curr.fileclient== EFileClient_Chunked)
		{
			ret = load_file_patch(curr);
		}

		if(!ret)
		{
			IScopedLock lock(mutex);
			is_offline=true;
		}
	}

	if(!is_offline && !skipping && client_main->getProtocolVersions().file_meta>0)
	{
		_u32 rc = fc.InformMetadataStreamEnd(server_token);

		if(rc!=ERR_SUCCESS)
		{
			ServerLogger::Log(logid, L"Error informing client about metadata stream end. Errorcode: "+widen(fc.getErrorString(rc))+L" ("+convert(rc)+L")", LL_ERROR);
		}
	}

	download_nok_ids.finalize();
	download_partial_ids.finalize();
}

void ServerDownloadThread::addToQueueFull(size_t id, const std::wstring &fn, const std::wstring &short_fn, const std::wstring &curr_path,
	const std::wstring &os_path, _i64 predicted_filesize, const FileMetadata& metadata,
    bool is_script, bool metadata_only, bool at_front )
{
	SQueueItem ni;
	ni.id = id;
	ni.fn = fn;
	ni.short_fn = short_fn;
	ni.curr_path = curr_path;
	ni.os_path = os_path;
	ni.fileclient = EFileClient_Full;
	ni.patch_dl_files.prepared=false;
	ni.patch_dl_files.prepare_error=false;
	ni.action = EQueueAction_Fileclient;
	ni.predicted_filesize = predicted_filesize;
	ni.metadata = metadata;
	ni.is_script = is_script;
    ni.metadata_only = metadata_only;

	IScopedLock lock(mutex);
	if(!at_front)
	{
		dl_queue.push_back(ni);
	}
	else
	{
		dl_queue.push_front(ni);
	}
	cond->notify_one();

	queue_size+=queue_items_full;
	if(!at_front)
	{
		sleepQueue(lock);
	}
}


void ServerDownloadThread::addToQueueChunked(size_t id, const std::wstring &fn, const std::wstring &short_fn,
	const std::wstring &curr_path, const std::wstring &os_path, _i64 predicted_filesize, const FileMetadata& metadata,
	bool is_script)
{
	SQueueItem ni;
	ni.id = id;
	ni.fn = fn;
	ni.short_fn = short_fn;
	ni.curr_path = curr_path;
	ni.os_path = os_path;
	ni.fileclient = EFileClient_Chunked;
	ni.patch_dl_files.prepared=false;
	ni.patch_dl_files.prepare_error=false;
	ni.action = EQueueAction_Fileclient;
	ni.predicted_filesize= predicted_filesize;
	ni.metadata = metadata;
	ni.is_script = is_script;
    ni.metadata_only = false;

	IScopedLock lock(mutex);
	dl_queue.push_back(ni);
	cond->notify_one();

	queue_size+=queue_items_chunked;
	sleepQueue(lock);
}

void ServerDownloadThread::addToQueueStartShadowcopy(const std::wstring& fn)
{
	SQueueItem ni;
	ni.action = EQueueAction_StartShadowcopy;
	ni.fn=fn;
	ni.id=std::string::npos;
	ni.patch_dl_files.prepared=false;
	ni.patch_dl_files.prepare_error=false;

	IScopedLock lock(mutex);
	dl_queue.push_back(ni);
	cond->notify_one();

	sleepQueue(lock);
}

void ServerDownloadThread::addToQueueStopShadowcopy(const std::wstring& fn)
{
	SQueueItem ni;
	ni.action = EQueueAction_StopShadowcopy;
	ni.fn=fn;
	ni.id=std::string::npos;
	ni.patch_dl_files.prepared=false;
	ni.patch_dl_files.prepare_error=false;

	IScopedLock lock(mutex);
	dl_queue.push_back(ni);
	cond->notify_one();

	sleepQueue(lock);
}


bool ServerDownloadThread::load_file(SQueueItem todl)
{
	ServerLogger::Log(logid, L"Loading file \""+todl.fn+L"\"", LL_DEBUG);
	IFile *fd=NULL;
    if(!todl.metadata_only)
	{
		fd = ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
		if(fd==NULL)
		{
			ServerLogger::Log(logid, L"Error creating temporary file 'fd' in load_file", LL_ERROR);
			return false;
		}
	}
	

	std::wstring cfn=getDLPath(todl);

    _u32 rc=fc.GetFile(Server->ConvertToUTF8(cfn), fd, hashed_transfer, todl.metadata_only);

	int hash_retries=5;
	while(rc==ERR_HASH && hash_retries>0)
	{
		fd->Seek(0);
        rc=fc.GetFile(Server->ConvertToUTF8(cfn), fd, hashed_transfer, todl.metadata_only);
		--hash_retries;
	}

	bool ret = true;
	bool hash_file = false;
	bool script_ok = true;

	if(rc!=ERR_SUCCESS)
	{
		ServerLogger::Log(logid, L"Error getting complete file \""+cfn+L"\" from "+clientname+L". Errorcode: "+widen(fc.getErrorString(rc))+L" ("+convert(rc)+L")", LL_ERROR);
		{
			IScopedLock lock(mutex);
			all_downloads_ok=false;
		}

		if( (rc==ERR_TIMEOUT || rc==ERR_ERROR)
			&& save_incomplete_file
            && fd!=NULL && fd->Size()>0
                && !todl.metadata_only)
		{
			ServerLogger::Log(logid, L"Saving incomplete file.", LL_INFO);
			hash_file = true;

			max_ok_id = (std::max)(max_ok_id, todl.id);
			download_partial_ids.add(todl.id);
		}
        else if(!todl.metadata_only)
		{
			download_nok_ids.add(todl.id);
			if(fd!=NULL)
			{
				ClientMain::destroyTemporaryFile(fd);
			}			
		}

		if(rc==ERR_TIMEOUT || rc==ERR_ERROR || rc==ERR_BASE_DIR_LOST)
		{
			ret=false;
		}
	}
	else
	{
		if(todl.is_script)
		{
			script_ok = logScriptOutput(cfn, todl);
		}

		max_ok_id = (std::max)(max_ok_id, todl.id);
		hash_file=true;
	}

    if(hash_file && !todl.metadata_only)
	{
		std::wstring os_curr_path=FileBackup::convertToOSPathFromFileClient(todl.os_path+L"/"+todl.short_fn);
		std::wstring os_curr_hash_path=FileBackup::convertToOSPathFromFileClient(todl.os_path+L"/"+escape_metadata_fn(todl.short_fn));
		std::wstring dstpath=backuppath+os_curr_path;
		std::wstring hashpath =backuppath_hashes+os_curr_hash_path;
		std::wstring filepath_old;
		
		if( use_reflink && (!last_backuppath.empty() || !last_backuppath_complete.empty() ) )
		{
			std::wstring cfn_short=todl.os_path+L"/"+todl.short_fn;
			if(cfn_short[0]=='/')
				cfn_short.erase(0,1);

			filepath_old=last_backuppath+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);

			IFile *file_old=Server->openFile(os_file_prefix(filepath_old), MODE_READ);

			if(file_old==NULL)
			{
				if(!last_backuppath_complete.empty())
				{
					filepath_old=last_backuppath_complete+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
					file_old=Server->openFile(os_file_prefix(filepath_old), MODE_READ);
				}
				if(file_old==NULL)
				{
					ServerLogger::Log(logid, L"No old file for \""+todl.fn+L"\"", LL_DEBUG);
					filepath_old.clear();
				}
			}

			Server->destroy(file_old);
		}

		hashFile(dstpath, hashpath, fd, NULL, Server->ConvertToUTF8(filepath_old), fd->Size(), todl.metadata, todl.is_script);
	}

	if(todl.is_script && (rc!=ERR_SUCCESS || !script_ok) )
	{
		return false;
	}

	return ret;
}

bool ServerDownloadThread::link_or_copy_file(SQueueItem todl)
{
	SPatchDownloadFiles dlfiles = todl.patch_dl_files;
	
	std::wstring os_curr_path=FileBackup::convertToOSPathFromFileClient(todl.os_path+L"/"+todl.short_fn);
	std::wstring dstpath=backuppath+os_curr_path;
	std::wstring dsthashpath = backuppath_hashes +os_curr_path;
	
	ScopedDeleteFile pfd_destroy(dlfiles.patchfile);
	ScopedDeleteFile hash_tmp_destroy(dlfiles.hashoutput);
	ScopedDeleteFile hashfile_old_destroy(NULL);
	ObjectScope file_old_destroy(dlfiles.orig_file);
	ObjectScope hashfile_old_delete(dlfiles.chunkhashes);

	if(dlfiles.delete_chunkhashes)
	{
		hashfile_old_destroy.reset(dlfiles.chunkhashes);
		hashfile_old_delete.release();
	}
	
	
	if( os_create_hardlink(os_file_prefix(dstpath), dlfiles.orig_file->getFilenameW(), use_reflink, NULL)
	    && os_create_hardlink(os_file_prefix(dsthashpath), dlfiles.chunkhashes->getFilenameW(), use_reflink, NULL) )
	{
		return true;
	}
	else
	{
		Server->deleteFile(os_file_prefix(dstpath));			
		
		bool ok = dlfiles.patchfile->Seek(0);
		int64 orig_filesize = dlfiles.orig_file->Size();
		int64 endian_filesize = little_endian(orig_filesize);
		ok = ok && (dlfiles.patchfile->Write(reinterpret_cast<char*>(&endian_filesize), sizeof(endian_filesize))==sizeof(endian_filesize));
			
		std::wstring hashfile_old_fn = dlfiles.chunkhashes->getFilenameW();
		std::wstring hashoutput_fn = dlfiles.hashoutput->getFilenameW();
		
		hash_tmp_destroy.release();
		delete dlfiles.hashoutput;
			
		if(ok && copy_file(hashfile_old_fn, hashoutput_fn) 
		    && (dlfiles.hashoutput=Server->openFile(hashoutput_fn, MODE_RW))!=NULL )
		{
			pfd_destroy.release();
			hashFile(dstpath, dlfiles.hashpath, dlfiles.patchfile, dlfiles.hashoutput,
			    Server->ConvertToUTF8(dlfiles.filepath_old), orig_filesize, todl.metadata, todl.is_script);
			return true;
		}
		else
		{
			hash_tmp_destroy.reset(dlfiles.hashoutput);
			return false;
		}
	}
}


bool ServerDownloadThread::load_file_patch(SQueueItem todl)
{
	std::wstring cfn=todl.curr_path+L"/"+todl.fn;
	if(cfn[0]=='/')
		cfn.erase(0,1);

	if(todl.is_script)
	{
		cfn = L"SCRIPT|" + cfn + L"|" + convert(incremental_num) + L"|" + convert(Server->getRandomNumber());
	}

	bool full_dl=false;
	SPatchDownloadFiles& dlfiles = todl.patch_dl_files;
	if(!dlfiles.prepared && !dlfiles.prepare_error)
	{
		dlfiles = preparePatchDownloadFiles(todl, full_dl);

		if(dlfiles.orig_file==NULL && full_dl)
		{
            addToQueueFull(todl.id, todl.fn, todl.short_fn, todl.curr_path, todl.os_path, todl.predicted_filesize, todl.metadata, todl.is_script, todl.metadata_only, true);
			return true;
		}
	}

	if(dlfiles.prepare_error)
	{
		return false;
	}


	ServerLogger::Log(logid, L"Loading file patch for \""+todl.fn+L"\"", LL_DEBUG);

	ScopedDeleteFile pfd_destroy(dlfiles.patchfile);
	ScopedDeleteFile hash_tmp_destroy(dlfiles.hashoutput);
	ScopedDeleteFile hashfile_old_destroy(NULL);
	ObjectScope file_old_destroy(dlfiles.orig_file);
	ObjectScope hashfile_old_delete(dlfiles.chunkhashes);

	if(dlfiles.delete_chunkhashes)
	{
		hashfile_old_destroy.reset(dlfiles.chunkhashes);
		hashfile_old_delete.release();
	}

	if(!server_token.empty() && !todl.is_script)
	{
		cfn=widen(server_token)+L"|"+cfn;
	}

	_u32 rc=fc_chunked->GetFilePatch(Server->ConvertToUTF8(cfn), dlfiles.orig_file, dlfiles.patchfile, dlfiles.chunkhashes, dlfiles.hashoutput, todl.predicted_filesize);

	int64 download_filesize = todl.predicted_filesize;

	int hash_retries=5;
	while(rc==ERR_HASH && hash_retries>0)
	{
		dlfiles.orig_file->Seek(0);
		dlfiles.patchfile=ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
		if(dlfiles.patchfile==NULL)
		{
			ServerLogger::Log(logid, L"Error creating temporary file 'pfd' in load_file_patch", LL_ERROR);
			return false;
		}
		pfd_destroy.reset(dlfiles.patchfile);
		dlfiles.hashoutput=ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
		if(dlfiles.hashoutput==NULL)
		{
			ServerLogger::Log(logid, L"Error creating temporary file 'hash_tmp' in load_file_patch -2", LL_ERROR);
			return false;
		}
		hash_tmp_destroy.reset(dlfiles.hashoutput);
		dlfiles.chunkhashes->Seek(0);
		download_filesize = todl.predicted_filesize;
		rc=fc_chunked->GetFilePatch(Server->ConvertToUTF8(cfn), dlfiles.orig_file, dlfiles.patchfile, dlfiles.chunkhashes, dlfiles.hashoutput, download_filesize);
		--hash_retries;
	} 

	if(download_filesize<0)
	{
		Server->Log("download_filesize is smaller than zero", LL_DEBUG);
		download_filesize=todl.predicted_filesize;
	}

	bool hash_file;

	bool script_ok = true;

	if(rc!=ERR_SUCCESS)
	{
		ServerLogger::Log(logid, L"Error getting file patch for \""+cfn+L"\" from "+clientname+L". Errorcode: "+widen(FileClient::getErrorString(rc))+L" ("+convert(rc)+L")", LL_ERROR);

		if(rc==ERR_ERRORCODES)
		{
			ServerLogger::Log(logid, "Remote Error: "+fc_chunked->getErrorcodeString(), LL_ERROR);
		}

		{
			IScopedLock lock(mutex);
			all_downloads_ok=false;
		}

		if( rc==ERR_BASE_DIR_LOST && save_incomplete_file)
		{
			ServerLogger::Log(logid, L"Saving incomplete file. (2)", LL_INFO);
			
			pfd_destroy.release();
			hash_tmp_destroy.release();
			hashfile_old_destroy.release();
			file_old_destroy.release();
			hashfile_old_delete.release();
			
			if(link_or_copy_file(todl))
			{
				max_ok_id = (std::max)(max_ok_id, todl.id);
				download_partial_ids.add(todl.id);
			}
			else
			{
				download_nok_ids.add(todl.id);				
			}
			
			hash_file=false;
		}
		else if( (rc==ERR_TIMEOUT || rc==ERR_CONN_LOST || rc==ERR_SOCKET_ERROR)
			&& dlfiles.patchfile->Size()>0
			&& save_incomplete_file)
		{
			ServerLogger::Log(logid, L"Saving incomplete file.", LL_INFO);
			hash_file=true;

			max_ok_id = (std::max)(max_ok_id, todl.id);
			download_partial_ids.add(todl.id);
		}
		else
		{
			hash_file=false;
			download_nok_ids.add(todl.id);
		}
	}
	else
	{
		if(todl.is_script)
		{
			script_ok = logScriptOutput(cfn, todl);
		}

		max_ok_id = (std::max)(max_ok_id, todl.id);
		hash_file=true;
	}

	if(hash_file)
	{
		std::wstring os_curr_path=FileBackup::convertToOSPathFromFileClient(todl.os_path+L"/"+todl.short_fn);		
		std::wstring dstpath=backuppath+os_curr_path;

		pfd_destroy.release();
		hash_tmp_destroy.release();
		hashFile(dstpath, dlfiles.hashpath, dlfiles.patchfile, dlfiles.hashoutput,
			Server->ConvertToUTF8(dlfiles.filepath_old), download_filesize, todl.metadata, todl.is_script);
	}

	if(todl.is_script && (rc!=ERR_SUCCESS || !script_ok) )
	{
		return false;
	}

	if(rc==ERR_TIMEOUT || rc==ERR_ERROR || rc==ERR_SOCKET_ERROR
		|| rc==ERR_INT_ERROR || rc==ERR_BASE_DIR_LOST || rc==ERR_CONN_LOST )
		return false;
	else
		return true;
}

void ServerDownloadThread::hashFile(std::wstring dstpath, std::wstring hashpath, IFile *fd, IFile *hashoutput, std::string old_file,
	int64 t_filesize, const FileMetadata& metadata, bool is_script)
{
	int l_backup_id=backupid;

	CWData data;
	data.addString(Server->ConvertToUTF8(fd->getFilenameW()));
	data.addInt(l_backup_id);
	data.addInt(r_incremental==true?1:0);
	data.addString(Server->ConvertToUTF8(dstpath));
	data.addString(Server->ConvertToUTF8(hashpath));
	if(hashoutput!=NULL)
	{
		data.addString(Server->ConvertToUTF8(hashoutput->getFilenameW()));
	}
	else
	{
		data.addString("");
	}

	data.addString(old_file);
	data.addInt64(t_filesize);
	metadata.serialize(data);

	ServerLogger::Log(logid, "GT: Loaded file \""+ExtractFileName(Server->ConvertToUTF8(dstpath))+"\"", LL_DEBUG);

	Server->destroy(fd);
	if(hashoutput!=NULL)
	{
		if(!is_script)
		{
			int64 expected_hashoutput_size = get_hashdata_size(t_filesize);
			if(hashoutput->Size()>expected_hashoutput_size)
			{
				std::wstring hashoutput_fn = hashoutput->getFilenameW();
				Server->destroy(hashoutput);
				os_file_truncate(hashoutput_fn, expected_hashoutput_size);			
			}
			else
			{
				Server->destroy(hashoutput);
			}
		}
		else
		{
			Server->destroy(hashoutput);
		}
		
	}
	hashpipe_prepare->Write(data.getDataPtr(), data.getDataSize() );
}

bool ServerDownloadThread::isOffline()
{
	IScopedLock lock(mutex);
	return is_offline;
}

void ServerDownloadThread::queueStop(bool immediately)
{
	SQueueItem ni;
	ni.action = EQueueAction_Quit;

	IScopedLock lock(mutex);
	if(immediately)
	{
		dl_queue.push_front(ni);
	}
	else
	{
		dl_queue.push_back(ni);
	}
	cond->notify_one();
}

bool ServerDownloadThread::isDownloadOk( size_t id )
{
	return !download_nok_ids.hasId(id);
}


bool ServerDownloadThread::isDownloadPartial( size_t id )
{
	return download_partial_ids.hasId(id);
}


size_t ServerDownloadThread::getMaxOkId()
{
	return max_ok_id;
}

std::string ServerDownloadThread::getQueuedFileFull(FileClient::MetadataQueue& metadata)
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && 
			!it->queued && it->fileclient==EFileClient_Full
			&& it->predicted_filesize>0)
		{
			it->queued=true;
			metadata=FileClient::MetadataQueue_Data;
			return Server->ConvertToUTF8(getDLPath(*it));
		}
	}

	return std::string();
}

std::wstring ServerDownloadThread::getDLPath( SQueueItem todl )
{
	std::wstring cfn=todl.curr_path+L"/"+todl.fn;
	if(cfn[0]=='/')
		cfn.erase(0,1);

	if(todl.is_script)
	{
		cfn = L"SCRIPT|" + cfn + L"|" + convert(incremental_num) + L"|" + convert(Server->getRandomNumber());
	}
	else if(!server_token.empty())
	{
		cfn=widen(server_token)+L"|"+cfn;
	}

	return cfn;
}

void ServerDownloadThread::resetQueueFull()
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && 
			it->fileclient==EFileClient_Full)
		{
			it->queued=false;
		}
	}
}

bool ServerDownloadThread::getQueuedFileChunked( std::string& remotefn, IFile*& orig_file, IFile*& patchfile, IFile*& chunkhashes, IFile*& hashoutput, _i64& predicted_filesize )
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && 
			!it->queued && it->fileclient==EFileClient_Chunked
			&& it->predicted_filesize>0)
		{
			if(it->patch_dl_files.prepare_error)
			{
				continue;
			}
			
			remotefn = Server->ConvertToUTF8(getDLPath(*it));


			if(!it->patch_dl_files.prepared)
			{
				bool full_dl;
				it->patch_dl_files = preparePatchDownloadFiles(*it, full_dl);

				if(it->patch_dl_files.orig_file==NULL &&
					full_dl)
				{
					it->fileclient=EFileClient_Full;
					queue_size-=queue_items_chunked-queue_items_full;
					continue;
				}
			}

			if(it->patch_dl_files.prepared)
			{
				it->queued=true;
				orig_file = it->patch_dl_files.orig_file;
				patchfile = it->patch_dl_files.patchfile;
				chunkhashes = it->patch_dl_files.chunkhashes;
				hashoutput = it->patch_dl_files.hashoutput;
				predicted_filesize = it->predicted_filesize;
				return true;
			}
		}
	}

	return false;
}

void ServerDownloadThread::resetQueueChunked()
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && it->fileclient==EFileClient_Chunked)
		{
			it->queued=false;
		}
	}
}

SPatchDownloadFiles ServerDownloadThread::preparePatchDownloadFiles( SQueueItem todl, bool& full_dl )
{
	SPatchDownloadFiles dlfiles = {};
	dlfiles.prepare_error=true;
	full_dl=false;

	std::wstring cfn=todl.curr_path+L"/"+todl.fn;
	if(cfn[0]=='/')
		cfn.erase(0,1);

	std::wstring cfn_short=todl.os_path+L"/"+todl.short_fn;
	if(cfn_short[0]=='/')
		cfn_short.erase(0,1);

	std::wstring dstpath=backuppath+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
	std::wstring hashpath=backuppath_hashes+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
	std::wstring hashpath_old=last_backuppath+os_file_sep()+L".hashes"+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
	std::wstring filepath_old=last_backuppath+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);

	std::auto_ptr<IFile> file_old(Server->openFile(os_file_prefix(filepath_old), MODE_READ));

	if(file_old.get()==NULL)
	{
		if(!last_backuppath_complete.empty())
		{
			filepath_old=last_backuppath_complete+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
			file_old.reset(Server->openFile(os_file_prefix(filepath_old), MODE_READ));
		}
		if(file_old.get()==NULL)
		{
			ServerLogger::Log(logid, L"No old file for \""+todl.fn+L"\"", LL_DEBUG);
			full_dl=true;
			return dlfiles;
		}
		hashpath_old=last_backuppath_complete+os_file_sep()+L".hashes"+os_file_sep()+FileBackup::convertToOSPathFromFileClient(cfn_short);
	}

	IFile *pfd=ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
	if(pfd==NULL)
	{
		ServerLogger::Log(logid, L"Error creating temporary file 'pfd' in load_file_patch", LL_ERROR);
		return dlfiles;
	}
	ScopedDeleteFile pfd_delete(pfd);
	IFile *hash_tmp=ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid);
	if(hash_tmp==NULL)
	{
		ServerLogger::Log(logid, L"Error creating temporary file 'hash_tmp' in load_file_patch", LL_ERROR);
		return dlfiles;
	}
	ScopedDeleteFile hash_tmp_delete(pfd);

	if(!server_token.empty())
	{
		cfn=widen(server_token)+L"|"+cfn;
	}

	std::auto_ptr<IFile> hashfile_old(Server->openFile(os_file_prefix(hashpath_old), MODE_READ));

	dlfiles.delete_chunkhashes=false;
	if( (hashfile_old.get()==NULL ||
		hashfile_old->Size()==0  ||
		is_metadata_only(hashfile_old.get()) ) 
		  && file_old.get()!=NULL )
	{
		ServerLogger::Log(logid, L"Hashes for file \""+filepath_old+L"\" not available. Calulating hashes...", LL_DEBUG);
		hashfile_old.reset(ClientMain::getTemporaryFileRetry(use_tmpfiles, tmpfile_path, logid));
		if(hashfile_old.get()==NULL)
		{
			ServerLogger::Log(logid, L"Error creating temporary file 'hashfile_old' in load_file_patch", LL_ERROR);
			return dlfiles;
		}
		dlfiles.delete_chunkhashes=true;
		build_chunk_hashs(file_old.get(), hashfile_old.get(), NULL, false, NULL, false);
		hashfile_old->Seek(0);
	}

	dlfiles.orig_file=file_old.release();
	dlfiles.patchfile=pfd;
	pfd_delete.release();
	dlfiles.chunkhashes=hashfile_old.release();
	dlfiles.hashoutput=hash_tmp;
	hash_tmp_delete.release();
	dlfiles.hashpath = hashpath;
	dlfiles.filepath_old = filepath_old;
	dlfiles.prepared=true;
	dlfiles.prepare_error=false;

	return dlfiles;
}

void ServerDownloadThread::start_shadowcopy(const std::string &path)
{
	client_main->sendClientMessage("START SC \""+path+"\"#token="+server_token, "DONE", L"Activating shadow copy on \""+clientname+L"\" for path \""+Server->ConvertToUnicode(path)+L"\" failed", shadow_copy_timeout);
}

void ServerDownloadThread::stop_shadowcopy(const std::string &path)
{
	client_main->sendClientMessage("STOP SC \""+path+"\"#token="+server_token, "DONE", L"Removing shadow copy on \""+clientname+L"\" for path \""+Server->ConvertToUnicode(path)+L"\" failed", shadow_copy_timeout);
}

void ServerDownloadThread::sleepQueue(IScopedLock& lock)
{
	while(queue_size>max_queue_size)
	{
		lock.relock(NULL);
		Server->wait(1000);
		lock.relock(mutex);
	}
}

void ServerDownloadThread::queueSkip()
{
	SQueueItem ni;
	ni.action = EQueueAction_Skip;

	IScopedLock lock(mutex);
	dl_queue.push_front(ni);
	cond->notify_one();
}

void ServerDownloadThread::unqueueFileFull( const std::string& fn )
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && 
			it->queued && it->fileclient==EFileClient_Full
			&& Server->ConvertToUTF8(getDLPath(*it)) == fn)
		{
			it->queued=false;
			return;
		}
	}
}

void ServerDownloadThread::unqueueFileChunked( const std::string& remotefn )
{
	IScopedLock lock(mutex);
	for(std::deque<SQueueItem>::iterator it=dl_queue.begin();
		it!=dl_queue.end();++it)
	{
		if(it->action==EQueueAction_Fileclient && 
			it->queued && it->fileclient==EFileClient_Chunked
			&& Server->ConvertToUTF8(getDLPath(*it)) == remotefn )
		{
			it->queued=false;
			return;
		}
	}
}

bool ServerDownloadThread::isAllDownloadsOk()
{
	IScopedLock lock(mutex);
	return all_downloads_ok;
}

bool ServerDownloadThread::logScriptOutput(std::wstring cfn, const SQueueItem &todl)
{
	std::string script_output = client_main->sendClientMessageRetry("SCRIPT STDERR "+Server->ConvertToUTF8(cfn),
		L"Error getting script output for command \""+todl.fn+L"\"", 10000, 10, true);

	if(script_output=="err")
	{
		ServerLogger::Log(logid, L"Error getting script output for command \""+todl.fn+L"\" (err response)", LL_ERROR);
		return false;
	}

	if(!script_output.empty())
	{
		int retval = atoi(getuntil(" ", script_output).c_str());

		std::vector<std::string> lines;
		Tokenize(getafter(" ", script_output), lines, "\n");

		for(size_t k=0;k<lines.size();++k)
		{
			ServerLogger::Log(logid, Server->ConvertToUTF8(todl.fn) + ": " + trim(lines[k]), retval!=0?LL_ERROR:LL_INFO);
		}

		if(retval!=0)
		{
			ServerLogger::Log(logid, L"Script \""+todl.fn+L"\" return a nun-null value "+convert(retval)+L". Failing backup.", LL_ERROR);
			return false;
		}
	}
	else
	{
		return false;
	}
	
	return true;
}
