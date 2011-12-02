#include "catalog_queries.h"

#include "Catalog.h"

using namespace std;

namespace cvmfs {

SqlStatement::SqlStatement(const sqlite3 *database, const std::string &statement) {
  Init(database, statement);
}

SqlStatement::~SqlStatement() {
  last_error_code_ = sqlite3_finalize(statement_);

  if (not Successful()) {
    pmesg(D_SQL, "FAILED to finalize statement - error code: %d", last_error_code_);
  }

  pmesg(D_SQL, "successfully finalized statement");
}

bool SqlStatement::Init(const sqlite3 *database, const std::string &statement) {
  last_error_code_ = sqlite3_prepare_v2((sqlite3*)database,
                                        statement.c_str(),
                                        -1, // parse until null termination
                                        &statement_,
                                        NULL);

  if (not Successful()) {
    pmesg(D_SQL, "FAILED to prepare statement '%s' - error code: %d", statement.c_str(), GetLastError());
    pmesg(D_SQL, "Error message: '%s'", sqlite3_errmsg((sqlite3*)database));
    return false;
  }

  pmesg(D_SQL, "successfully prepared statement '%s'", statement.c_str());
  return true;
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

unsigned int DirectoryEntrySqlStatement::CreateDatabaseFlags(const DirectoryEntry &entry) const {
  unsigned int database_flags = 0;
  
  if (entry.IsNestedCatalogRoot()) {
    database_flags |= kFlagDirNestedRoot;
  }
  
  if (entry.IsNestedCatalogMountpoint()) {
    database_flags |= kFlagDirNestedMountpoint;
  }
  
  if (entry.IsDirectory()) {
    database_flags |= kFlagDir;
  } else if (entry.IsLink()) {
    database_flags |= kFlagFile | kFlagLink;
  } else {
    database_flags |= kFlagFile;
  }
  
  database_flags = SetLinkcountInFlags(database_flags, entry.linkcount());
  return database_flags;
}

std::string DirectoryEntrySqlStatement::ExpandSymlink(const std::string raw_symlink) const {
  string result = "";
   
  for (string::size_type i = 0; i < raw_symlink.length(); i++) {
    string::size_type lpar;
    string::size_type rpar;
    if ((raw_symlink[i] == '$') && 
        ((lpar = raw_symlink.find('(', i+1)) != string::npos) &&
        ((rpar = raw_symlink.find(')', i+2)) != string::npos) &&
        (rpar > lpar))  
    {
      string var = raw_symlink.substr(lpar + 1, rpar-lpar-1);
      char *var_exp = getenv(var.c_str()); /* Don't free! Nothing is allocated here */
      if (var_exp) {
        result += var_exp;
      }
      i = rpar;
    } else {
      result += raw_symlink[i];
    }
  }

  return result;
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

std::string LookupSqlStatement::GetFieldsToSelect() const {
  return "hash, inode, size, mode, mtime, flags, name, symlink, md5path_1, md5path_2, parent_1, parent_2, rowid";
      //    0     1      2     3     4      5      6      7        8          9           10        11       12
}

hash::t_md5 LookupSqlStatement::GetPathHash() const {
  return RetrieveMd5Hash(8, 9);
}

hash::t_md5 LookupSqlStatement::GetParentPathHash() const {
  return RetrieveMd5Hash(10, 11);
}

DirectoryEntry LookupSqlStatement::GetDirectoryEntry(const Catalog *catalog) const {
  // fill the directory entry
  // (this method is a friend of DirectoryEntry ;-) )
  DirectoryEntry result;
  
  // read administrative stuff from the result
  int database_flags                   = RetrieveInt(5);
  result.catalog_                      = (Catalog*)catalog;
  result.is_nested_catalog_root_       = (database_flags & kFlagDirNestedRoot);
  result.is_nested_catalog_mountpoint_ = (database_flags & kFlagDirNestedMountpoint);
  result.hardlink_group_id_            = RetrieveInt64(1); // quirky database layout here ( legacy ;-) )
  
  // read the usual file information
  result.inode_        = ((Catalog*)catalog)->GetInodeFromRowIdAndHardlinkGroupId(RetrieveInt64(12), RetrieveInt64(1));
  result.parent_inode_ = DirectoryEntry::kInvalidInode; // must be set later by a second catalog lookup
  result.linkcount_    = GetLinkcountFromFlags(database_flags);
  result.mode_         = RetrieveInt(3);
  result.size_         = RetrieveInt64(2);
  result.mtime_        = RetrieveInt64(4);
  result.checksum_     = RetrieveSha1Hash(0);
  result.name_         = string((char *)RetrieveText(6));
  result.symlink_      = ExpandSymlink((char *)RetrieveText(7));
  
  return result;
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

ListingLookupSqlStatement::ListingLookupSqlStatement(const sqlite3 *database) {
  std::ostringstream statement;
  statement << "SELECT " << GetFieldsToSelect() << " FROM catalog "
               "WHERE (parent_1 = :p_1) AND (parent_2 = :p_2);";
  Init(database, statement.str());
}

bool ListingLookupSqlStatement::BindPathHash(const struct hash::t_md5 &hash) {
  return BindMd5Hash(1, 2, hash);
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

PathHashLookupSqlStatement::PathHashLookupSqlStatement(const sqlite3 *database) {
  std::ostringstream statement;
  statement << "SELECT " << GetFieldsToSelect() << " FROM catalog "
               "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);";
  Init(database, statement.str());
}

bool PathHashLookupSqlStatement::BindPathHash(const struct hash::t_md5 &hash) {
  return BindMd5Hash(1, 2, hash);
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

InodeLookupSqlStatement::InodeLookupSqlStatement(const sqlite3 *database) {
  std::ostringstream statement;
  statement << "SELECT " << GetFieldsToSelect() << " FROM catalog "
               "WHERE rowid = :rowid;";
  Init(database, statement.str());
}

bool InodeLookupSqlStatement::BindRowId(const uint64_t inode) {
  return BindInt64(1, inode);
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

FindNestedCatalogSqlStatement::FindNestedCatalogSqlStatement(const sqlite3 *database) {
  Init(database, "SELECT sha1 FROM nested_catalogs WHERE path=:path;");
}

bool FindNestedCatalogSqlStatement::BindSearchPath(const std::string &path) {
  return BindText(1, &path[0], path.length(), SQLITE_STATIC);
}

hash::t_sha1 FindNestedCatalogSqlStatement::GetContentHash() const {
  hash::t_sha1 sha1;
  const std::string sha1_str = std::string((char *)RetrieveText(0));
  sha1.from_hash_str(sha1_str);
  return sha1;
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

InsertDirectoryEntrySqlStatement::InsertDirectoryEntrySqlStatement(const sqlite3 *database) {
  Init(database, "INSERT INTO catalog "
  //                  1           2         3         4       5     6      7     8      9      10    11     12
                 "(md5path_1, md5path_2, parent_1, parent_2, hash, inode, size, mode, mtime, flags, name, symlink) "
                 "VALUES (:md5_1, :md5_2, :p_1, :p_2, :hash, :ino, :size, :mode, :mtime, :flags, :name, :symlink);");
}

bool InsertDirectoryEntrySqlStatement::BindPathHash(const hash::t_md5 &hash) {
  return BindMd5Hash(1, 2, hash);
}

bool InsertDirectoryEntrySqlStatement::BindParentPathHash(const hash::t_md5 &hash) {
  return BindMd5Hash(3, 4, hash);
}

bool InsertDirectoryEntrySqlStatement::BindDirectoryEntry(const DirectoryEntry &entry) {
  return (
    BindSha1Hash(5, entry.checksum_) &&
    BindInt64(6, entry.hardlink_group_id_) && // quirky database layout here ( legacy ;-) )
    BindInt64(7, entry.size_) &&
    BindInt(8, entry.mode_) &&
    BindInt64(9, entry.mtime_) &&
    BindInt(10, CreateDatabaseFlags(entry)) &&
    BindText(11, &entry.name_[0], entry.name_.length(), SQLITE_STATIC) &&
    BindText(12, &entry.symlink_[0], entry.symlink_.length(), SQLITE_STATIC)
  );
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

TouchSqlStatement::TouchSqlStatement(const sqlite3 *database) {
  Init(database, "UPDATE catalog SET mtime = :mtime "
                 "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}

bool TouchSqlStatement::BindPathHash(const hash::t_md5 &hash) {
  return BindMd5Hash(2, 3, hash);
}

bool TouchSqlStatement::BindTimestamp(const time_t timestamp) {
  return BindInt64(1, timestamp);
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

UnlinkSqlStatement::UnlinkSqlStatement(const sqlite3 *database) {
  Init(database, "DELETE FROM catalog "
                 "WHERE (md5path_1 = :md5_1) AND (md5path_2 = :md5_2);");
}

bool UnlinkSqlStatement::BindPathHash(const hash::t_md5 &hash) {
  return BindMd5Hash(1, 2, hash);
}

//
// ###########################################################################
// ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###   ###
// ###########################################################################
//

GetMaximalHardlinkGroupIdStatement::GetMaximalHardlinkGroupIdStatement(const sqlite3 *database) {
  Init(database, "SELECT max(inode) FROM catalog;");
}

int GetMaximalHardlinkGroupIdStatement::GetMaximalGroupId() const {
  return RetrieveInt64(0);
}


} // namespace cvmfs
