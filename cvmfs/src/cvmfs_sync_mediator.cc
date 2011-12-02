#include "cvmfs_sync_mediator.h"

extern "C" {
   #include "compression.h"
   #include "smalloc.h"
}

#include "hash.h"
#include "cvmfs_sync_aufs.h"
#include "cvmfs_sync_recursion.h"

#include <sstream>
#include <iostream> // TODO: remove that!

using namespace cvmfs;
using namespace std;

SyncMediator::SyncMediator(WritableCatalogManager *catalogManager, const SyncParameters *parameters) {
	mCatalogManager = catalogManager;
	mDataDirectory = canonical_path(parameters->dir_data);
	mDryRun = parameters->dry_run;
	mPrintChangeset = parameters->print_changeset;
}

SyncMediator::~SyncMediator() {
	
}

void SyncMediator::add(DirEntry *entry) {
	if (entry->isDirectory()) {
		addDirectoryRecursively(entry);
	}
	
	else if (entry->isRegularFile() || entry->isSymlink()) {

		// create a nested catalog if we find a NEW request file
		if (entry->isCatalogRequestFile() && entry->isNew()) {
			createNestedCatalog(entry);
		}
		
		// a file is a hard link if the link count is greater than 1
		if (entry->getUnionLinkcount() > 1) {
			insertHardlink(entry);
		} else {
			addFile(entry);
		}
	}
	
	else {
		stringstream ss;
		ss << "'" << entry->getRelativePath() << "' cannot be added. Unregcognized file format.";
		printWarning(ss.str());
	}
}

void SyncMediator::touch(DirEntry *entry) {
	if (entry->isDirectory()) {
		touchDirectory(entry);
	}
	
	else if (entry->isRegularFile() || entry->isSymlink()) {
		replace(entry);
	}

	else {
		stringstream ss;
		ss << "'" << entry->getRelativePath() << "' cannot be touched. Unregcognized file format.";
		printWarning(ss.str());
	}
}

void SyncMediator::remove(DirEntry *entry) {
	if (entry->isDirectory()) {
		removeDirectoryRecursively(entry);
	}
	
	else if (entry->isRegularFile() || entry->isSymlink()) {
		// first remove the file...
		removeFile(entry);
		
		// ... then the nested catalog (if needed)
		if (entry->isCatalogRequestFile() && not entry->isNew()) {
			removeNestedCatalog(entry);
		}
	}

	else {
		stringstream ss;
		ss << "'" << entry->getRelativePath() << "' cannot be deleted. Unregcognized file format.";
		printWarning(ss.str());
	}
}

void SyncMediator::replace(DirEntry *entry) {
	// an entry is just a representation of a filename
	// replacing it is as easy as that:
	remove(entry);
	add(entry);
}

void SyncMediator::enterDirectory(DirEntry *entry) {
	HardlinkGroupMap newMap;
	mHardlinkStack.push(newMap);
}

void SyncMediator::leaveDirectory(DirEntry *entry) {
	completeHardlinks(entry);
	addHardlinkGroups(getHardlinkMap());
   cleanupHardlinkGroups(getHardlinkMap());
	mHardlinkStack.pop();
}

void SyncMediator::leaveAddedDirectory(DirEntry *entry) {
	addHardlinkGroups(getHardlinkMap());
   cleanupHardlinkGroups(getHardlinkMap());
	mHardlinkStack.pop();
}

void SyncMediator::commit() {
	compressAndHashFileQueue();
	addFileQueueToCatalogs();
   releaseFileQueue();
	mCatalogManager->PrecalculateListings();
	mCatalogManager->Commit();
}

void SyncMediator::compressAndHashFileQueue() {
	// compressing and hashing files
	// TODO: parallelize this!
	DirEntryList::iterator i;
	DirEntryList::const_iterator iend;
	for (i = mFileQueue.begin(), iend = mFileQueue.end(); i != iend; ++i) {
		hash::t_sha1 hash;
		addFileToDatastore(*i, hash);
		(*i)->setContentHash(hash);
	}

	// compressing and hashing files in hardlink groups
	// (hardlinks point to the same "data", therefore we only have to compress it once)
	HardlinkGroupList::iterator j;
	HardlinkGroupList::const_iterator jend;
	DirEntryList::iterator k;
	DirEntryList::const_iterator kend;
	for (j = mHardlinkQueue.begin(), jend = mHardlinkQueue.end(); j != jend; ++j) {
		// hardlinks to anything else (mostly symlinks) do not have to be compressed
		if (not j->masterFile->isRegularFile()) {
			continue;
		}
		
		// compress the master file
		hash::t_sha1 hash;
		addFileToDatastore(j->masterFile, hash);
		
		// distribute the obtained hash for every hardlink
		for (k = j->hardlinks.begin(), kend = j->hardlinks.end(); k != kend; ++k) {
			(*k)->setContentHash(hash);
		}
	}
}

void SyncMediator::addFileQueueToCatalogs() {
	// don't do things you could regret later on
	if (mDryRun) {
		return;
	}
	
	// add singular files
	DirEntryList::iterator i;
	DirEntryList::const_iterator iend;
	for (i = mFileQueue.begin(), iend = mFileQueue.end(); i != iend; ++i) {
		mCatalogManager->AddFile(*i);
	}
	
	// add hardlink groups
	HardlinkGroupList::iterator j;
	HardlinkGroupList::const_iterator jend;
	for (j = mHardlinkQueue.begin(), jend = mHardlinkQueue.end(); j != jend; ++j) {
		mCatalogManager->AddHardlinkGroup(j->hardlinks);
	}
}

void SyncMediator::releaseFileQueue() {
   // release DirEntries from file queue
	DirEntryList::iterator i;
	DirEntryList::const_iterator iend;
	for (i = mFileQueue.begin(), iend = mFileQueue.end(); i != iend; ++i) {
		(*i)->release();
	}
	
	// release DirEntries from hardlink groups (hardlinks to symlinks are already wiped at this place)
	HardlinkGroupList::iterator j;
	HardlinkGroupList::const_iterator jend;
	for (j = mHardlinkQueue.begin(), jend = mHardlinkQueue.end(); j != jend; ++j) {
      for (i = j->hardlinks.begin(), iend = j->hardlinks.end(); i != iend; ++i) {
         (*i)->release();
      }
	}
}

bool SyncMediator::addFileToDatastore(DirEntry *entry, const std::string &suffix, hash::t_sha1 &hash) {
	// don't do that, would change something!
	if (mDryRun) {
		return true;
	}
	
	bool result = false;
	/* Create temporary file */
	const string templ = mDataDirectory + "/txn/compressing.XXXXXX";
	char *tmp_path = (char *)smalloc(templ.length() + 1);
	strncpy(tmp_path, templ.c_str(), templ.length() + 1);
	int fd_dst = mkstemp(tmp_path);

	if ((fd_dst >= 0) && (fchmod(fd_dst, plain_file_mode) == 0)) {
		/* Compress and calculate SHA1 */
		FILE *fsrc = NULL, *fdst = NULL;
		if ( (fsrc = fopen(entry->getOverlayPath().c_str(), "r")) && 
		     (fdst = fdopen(fd_dst, "w")) && 
		     (compress_file_fp_sha1(fsrc, fdst, hash.digest) == 0) )
		{
			const string sha1str = hash.to_string();
			const string cache_path = mDataDirectory + "/" + sha1str.substr(0, 2) + "/" + 
			sha1str.substr(2) + suffix;
			fflush(fdst);
			if (rename(tmp_path, cache_path.c_str()) == 0) {
				result = true;
			} else {	
				unlink(tmp_path);
				stringstream ss;
				ss << "could not rename " << tmp_path << " to " << cache_path;
				printWarning(ss.str());
			}
		} else {	
			stringstream ss;
			ss << "could not compress " << entry->getOverlayPath();
			printWarning(ss.str());
		}
		if (fsrc) fclose(fsrc);
		if (fdst) fclose(fdst);
	} else {
		stringstream ss;
		ss << "could not create temporary file " << templ;
		printWarning(ss.str());
		result = false;
	}
	free(tmp_path);

	return result;
}

void SyncMediator::insertHardlink(DirEntry *entry) {
	uint64_t inode = entry->getUnionInode();
	
	// find the hard link group in the lists
	HardlinkGroupMap::iterator hardlinkGroup = getHardlinkMap().find(inode);

   // this DirEntry will stay for some time... increment reference counter
   entry->retain();

	if (hardlinkGroup == getHardlinkMap().end()) {
		// create a new hardlink group
		HardlinkGroup newGroup;
		newGroup.masterFile = entry;
		newGroup.hardlinks.push_back(entry);
		getHardlinkMap()[inode] = newGroup;
	} else {
		// append the file to the appropriate hardlink group
		hardlinkGroup->second.hardlinks.push_back(entry);
	}
}

void SyncMediator::insertExistingHardlink(DirEntry *entry) {
	// check if found file has hardlinks (nlink > 1)
	// as we are looking through all files in one directory here, there might be
	// completely untouched hardlink groups, which we can safely skip
	// finally we have to see, if the hardlink is already part of this group
	
	// check if we have a hard link here
	if (entry->getUnionLinkcount() <= 1) {
		return;
	}
	
	uint64_t inode = entry->getUnionInode();
	HardlinkGroupMap::iterator hlGroup;
	hlGroup = getHardlinkMap().find(inode);

	if (hlGroup != getHardlinkMap().end()) { // touched hardlinks in this group?
		DirEntryList::const_iterator i,end;
		bool alreadyThere = false;
		
		// search for the entry in this group
		for (i = hlGroup->second.hardlinks.begin(), end = hlGroup->second.hardlinks.end(); i != end; ++i) {
			if ((*i)->isEqualTo(entry)) {
				alreadyThere = true;
				break;
			}
		}
		
		if (not alreadyThere) { // hardlink already in the group?
			// if one element of a hardlink group is edited, all elements must be replaced
			// here we remove an untouched hardlink and add it to its hardlink group for re-adding later
			// (don't forget to increase the reference count for this)
			remove(entry);
         entry->retain();
			hlGroup->second.hardlinks.push_back(entry);
		}
	}
}

void SyncMediator::completeHardlinks(DirEntry *entry) {
	// create a recursion engine which DOES NOT recurse into directories by default.
	// it basically goes through the current directory (in the union volume) and
	// searches for legacy hardlinks which has to be connected to the new (edited) ones
	
	// if no hardlink in this directory was changed, we can skip this
	if (getHardlinkMap().size() == 0) {
		return;
	}
	
	// look for legacy hardlinks
	RecursionEngine<SyncMediator> recursion(this, UnionSync::sharedInstance()->getUnionPath(), UnionSync::sharedInstance()->getIgnoredFilenames(), false);
	recursion.foundRegularFile = &SyncMediator::insertExistingHardlink;
	recursion.foundSymlink = &SyncMediator::insertExistingHardlink;
	recursion.recurse(entry->getUnionPath());
}

void SyncMediator::addDirectoryRecursively(DirEntry *entry) {
	addDirectory(entry);
	
	// create a recursion engine, which recursively adds all entries in a newly created directory
	RecursionEngine<SyncMediator> recursion(this, UnionSync::sharedInstance()->getOverlayPath(), UnionSync::sharedInstance()->getIgnoredFilenames());
	recursion.enteringDirectory = &SyncMediator::enterDirectory;
	recursion.leavingDirectory = &SyncMediator::leaveAddedDirectory;
	recursion.foundRegularFile = &SyncMediator::add;
	recursion.foundDirectory = &SyncMediator::addDirectoryCallback;
	recursion.foundSymlink = &SyncMediator::add;
	recursion.recurse(entry->getOverlayPath());
}

void SyncMediator::removeDirectoryRecursively(DirEntry *entry) {
	RecursionEngine<SyncMediator> recursion(this, UnionSync::sharedInstance()->getRepositoryPath(), UnionSync::sharedInstance()->getIgnoredFilenames());
	recursion.foundRegularFile = &SyncMediator::remove;
	// delete a directory AFTER it was emptied (we cannot use the generic SyncMediator::remove() here, because it would start up another recursion)
	recursion.foundDirectoryAfterRecursion = &SyncMediator::removeDirectory;
	recursion.foundSymlink = &SyncMediator::remove;
	recursion.recurse(entry->getRepositoryPath());
	
	removeDirectory(entry);
}

RecursionPolicy SyncMediator::addDirectoryCallback(DirEntry *entry) {
	addDirectory(entry);
	return RP_RECURSE; // <-- tells the recursion engine to recurse further into this directory
}

void SyncMediator::createNestedCatalog(DirEntry *requestFile) {
	if (mPrintChangeset) cout << "[add] NESTED CATALOG" << endl;
	if (not mDryRun)     mCatalogManager->CreateNestedCatalog(requestFile->getParentPath());
}

void SyncMediator::removeNestedCatalog(DirEntry *requestFile) {
	if (mPrintChangeset) cout << "[rem] NESTED CATALOG" << endl;
	if (not mDryRun)     mCatalogManager->RemoveNestedCatalog(requestFile->getParentPath());
}

void SyncMediator::addFile(DirEntry *entry) {
	if (mPrintChangeset) cout << "[add] " << entry->getRepositoryPath() << endl;
	
	if (entry->isSymlink() && not mDryRun) {
	   // symlinks have no 'actual' file content, which would have to be compressed...
		mCatalogManager->AddFile(entry);
	} else {
	   // a normal file has content, that has to be compressed later in the commit-stage
	   // keep the entry in memory!
      entry->retain();
		mFileQueue.push_back(entry);
	}
}

void SyncMediator::removeFile(DirEntry *entry) {
	if (mPrintChangeset) cout << "[rem] " << entry->getRepositoryPath() << endl;
	if (not mDryRun)     mCatalogManager->RemoveFile(entry);
}

void SyncMediator::touchFile(DirEntry *entry) {
	if (mPrintChangeset) cout << "[tou] " << entry->getRepositoryPath() << endl;
	if (not mDryRun)     mCatalogManager->TouchFile(entry);
}

void SyncMediator::addDirectory(DirEntry *entry) {
	if (mPrintChangeset) cout << "[add] " << entry->getRepositoryPath() << endl;
	if (not mDryRun)     mCatalogManager->AddDirectory(entry);
}

void SyncMediator::removeDirectory(DirEntry *entry) {
	if (mPrintChangeset) cout << "[rem] " << entry->getRepositoryPath() << endl;
	if (not mDryRun)     mCatalogManager->RemoveDirectory(entry);
}

void SyncMediator::touchDirectory(DirEntry *entry) {
	if (mPrintChangeset) cout << "[tou] " << entry->getRepositoryPath() << endl;
	if (not mDryRun)     mCatalogManager->TouchDirectory(entry);
}

void SyncMediator::addHardlinkGroups(const HardlinkGroupMap &hardlinks) {
   HardlinkGroupMap::const_iterator i,end;
	for (i = hardlinks.begin(), end = hardlinks.end(); i != end; ++i) {
	   // currentHardlinkGroup = i->second; --> just to remind you
	   
		if (mPrintChangeset) {
			cout << "[add] hardlink group around: " << i->second.masterFile->getRepositoryPath() << "( ";	
			DirEntryList::const_iterator j,jend;
			for (j = i->second.hardlinks.begin(), jend = i->second.hardlinks.end(); j != jend; ++j) {
				cout << (*j)->getFilename() << " ";
			}
			cout << ")" << endl;
		}
		
		if (i->second.masterFile->isSymlink() && not mDryRun) {
		   // symlink hardlinks just end up in the database (see SyncMediator::addFile() same semantics here)
			mCatalogManager->AddHardlinkGroup(i->second.hardlinks);
         
		} else {
		   // yeah... just see SyncMediator::addFile()
			mHardlinkQueue.push_back(i->second);
		}
	}
}

void SyncMediator::cleanupHardlinkGroups(HardlinkGroupMap &hardlinks) {
   HardlinkGroupMap::iterator i;
   HardlinkGroupMap::const_iterator iend;
   DirEntryList::const_iterator j;
   DirEntryList::const_iterator jend;
	for (i = hardlinks.begin(), iend = hardlinks.end(); i != iend; ++i) {
	   // currentHardlinkGroup = i->second; --> just to remind you
		
		if (i->second.masterFile->isSymlink()) {
			// symlinks were just added to the database, their DirEntries can die afterwards (see SyncMediator::addHardlinkGroups)
         for (j = i->second.hardlinks.begin(), jend = i->second.hardlinks.end(); j != jend; ++j) {
            (*j)->release();
         }
		}
	}
}
