/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "Restore.hpp"
#include "BackupFormat.hpp"
#include <NdbTCP.h>
#include <OutputStream.hpp>
#include <Bitmask.hpp>

#include <AttributeHeader.hpp>
#include <trigger_definitions.h>
#include <SimpleProperties.hpp>
#include <signaldata/DictTabInfo.hpp>

// from src/ndbapi
#include <NdbDictionaryImpl.hpp>

Uint16 Twiddle16(Uint16 in); // Byte shift 16-bit data
Uint32 Twiddle32(Uint32 in); // Byte shift 32-bit data
Uint64 Twiddle64(Uint64 in); // Byte shift 64-bit data

bool
BackupFile::Twiddle(AttributeS* attr, Uint32 arraySize){

  if(m_hostByteOrder)
    return true;
  
  if(arraySize == 0){
    arraySize = attr->Desc->arraySize;
  }
  
  switch(attr->Desc->size){
  case 8:
    
    return true;
  case 16:
    for(unsigned i = 0; i<arraySize; i++){
      attr->Data.u_int16_value[i] = Twiddle16(attr->Data.u_int16_value[i]);
    }
    return true;
  case 32:
    for(unsigned i = 0; i<arraySize; i++){
      attr->Data.u_int32_value[i] = Twiddle32(attr->Data.u_int32_value[i]);
    }
    return true;
  case 64:
    for(unsigned i = 0; i<arraySize; i++){
      attr->Data.u_int64_value[i] = Twiddle64(attr->Data.u_int64_value[i]);
    }
    return true;
  default:
    return false;
  } // switch

} // Twiddle

FilteredNdbOut err(* new FileOutputStream(stderr), 0, 0);
FilteredNdbOut info(* new FileOutputStream(stdout), 1, 1);
FilteredNdbOut debug(* new FileOutputStream(stdout), 2, 0);

// To decide in what byte order data is
const Uint32 magicByteOrder = 0x12345678;
const Uint32 swappedMagicByteOrder = 0x78563412;

RestoreMetaData::RestoreMetaData(const char* path, Uint32 nodeId, Uint32 bNo) {
  
  debug << "RestoreMetaData constructor" << endl;
  setCtlFile(nodeId, bNo, path);
}

RestoreMetaData::~RestoreMetaData(){
  for(int i = 0; i<allTables.size(); i++)
    delete allTables[i];
  allTables.clear();
}

const TableS * 
RestoreMetaData::getTable(Uint32 tableId) const {
  for(int i = 0; i<allTables.size(); i++)
    if(allTables[i]->getTableId() == tableId)
      return allTables[i];
  return NULL;
}

Uint32
RestoreMetaData::getStopGCP() const {
  return m_stopGCP;
}

int
RestoreMetaData::loadContent() 
{
  Uint32 noOfTables = readMetaTableList();
  if(noOfTables == 0) {
    return 1;
  }
  for(Uint32 i = 0; i<noOfTables; i++){
    if(!readMetaTableDesc()){
      return 0;
    }
  }
  if(!readGCPEntry())
    return 0;
  return 1;
}

Uint32
RestoreMetaData::readMetaTableList() {
  
  Uint32 sectionInfo[2];
  
  if (fread(&sectionInfo, sizeof(sectionInfo), 1, m_file) != 1){
    return 0;
  }
  sectionInfo[0] = ntohl(sectionInfo[0]);
  sectionInfo[1] = ntohl(sectionInfo[1]);

  const Uint32 tabCount = sectionInfo[1] - 2;

  const Uint32 len = 4 * tabCount;
  if(createBuffer(len) == 0)
    abort();

  if (fread(m_buffer, 1, len, m_file) != len){
    return 0;
  }
  
  return tabCount;
}

bool
RestoreMetaData::readMetaTableDesc() {
  
  Uint32 sectionInfo[2];
  
  // Read section header 
  if (fread(&sectionInfo, sizeof(sectionInfo), 1, m_file) != 1){
    err << "readMetaTableDesc read header error" << endl;
    return false;
  } // if
  sectionInfo[0] = ntohl(sectionInfo[0]);
  sectionInfo[1] = ntohl(sectionInfo[1]);
  
  assert(sectionInfo[0] == BackupFormat::TABLE_DESCRIPTION);
  
  // Allocate temporary storage for dictTabInfo buffer
  const Uint32 len = (sectionInfo[1] - 2);
  if (createBuffer(4 * (len+1)) == NULL) {
    err << "readMetaTableDesc allocation error" << endl;
    return false;
  } // if
  
  // Read dictTabInfo buffer
  if (fread(m_buffer, 4, len, m_file) != len){
    err << "readMetaTableDesc read error" << endl;
    return false;
  } // if
  
  return parseTableDescriptor(m_buffer, len);	     
}

bool
RestoreMetaData::readGCPEntry() {

  Uint32 data[4];
  
  
  BackupFormat::CtlFile::GCPEntry * dst = 
    (BackupFormat::CtlFile::GCPEntry *)&data[0];
  
  if(fread(dst, 4, 4, m_file) != 4){
    err << "readGCPEntry read error" << endl;
    return false;
  }
  
  dst->SectionType = ntohl(dst->SectionType);
  dst->SectionLength = ntohl(dst->SectionLength);
  
  if(dst->SectionType != BackupFormat::GCP_ENTRY){
    err << "readGCPEntry invalid format" << endl;
    return false;
  }
  
  dst->StartGCP = ntohl(dst->StartGCP);
  dst->StopGCP = ntohl(dst->StopGCP);
  
  m_startGCP = dst->StartGCP;
  m_stopGCP = dst->StopGCP;
  return true;
}

TableS::TableS(NdbTableImpl* tableImpl)
  : m_dictTable(tableImpl)
{
  m_dictTable = tableImpl;
  m_noOfNullable = m_nullBitmaskSize = 0;

  for (Uint32 i = 0; i < tableImpl->getNoOfColumns(); i++)
    createAttr(tableImpl->getColumn(i));
}

// Parse dictTabInfo buffer and pushback to to vector storage 
bool
RestoreMetaData::parseTableDescriptor(const Uint32 * data, Uint32 len)
{
  NdbTableImpl* tableImpl = 0;
  int ret = NdbDictInterface::parseTableInfo(&tableImpl, data, len, false);

  if (ret != 0) {
    err << "parseTableInfo " << " failed" << endl;
    return false;
  }
  if(tableImpl == 0)
    return false;

  debug << "parseTableInfo " << tableImpl->getName() << " done" << endl;

  TableS * table = new TableS(tableImpl);
  if(table == NULL) {
    return false;
  }
  table->setBackupVersion(m_fileHeader.NdbVersion);

  debug << "Parsed table id " << table->getTableId() << endl;
  debug << "Parsed table #attr " << table->getNoOfAttributes() << endl;
  debug << "Parsed table schema version not used " << endl;

  debug << "Pushing table " << table->getTableName() << endl;
  debug << "   with " << table->getNoOfAttributes() << " attributes" << endl;
  allTables.push_back(table);

  return true;
}

// Constructor
RestoreDataIterator::RestoreDataIterator(const RestoreMetaData & md)
  : m_metaData(md) 
{
  debug << "RestoreDataIterator constructor" << endl;
  setDataFile(md, 0);
}

RestoreDataIterator::~RestoreDataIterator(){
}

bool
TupleS::prepareRecord(const TableS & tab){
  m_currentTable = &tab;
  for(int i = 0; i<allAttributes.size(); i++) {
    if(allAttributes[i] != NULL)
      delete allAttributes[i];
  }
  allAttributes.clear();
  AttributeS * a;
  for(int i = 0; i<tab.getNoOfAttributes(); i++){
    a = new AttributeS;
    if(a == NULL) {
      ndbout_c("Restore: Failed to allocate memory");
      return false;
    }
    a->Desc = tab[i];
    allAttributes.push_back(a);
  }
  return true;
}

const TupleS *
RestoreDataIterator::getNextTuple(int  & res) {
  TupleS * tup = new TupleS();
  if(tup == NULL) {
    ndbout_c("Restore: Failed to allocate memory");
    res = -1;
    return NULL;
  }
  if(!tup->prepareRecord(* m_currentTable)) {
    res =-1;
    return NULL;
  }
    

  Uint32  dataLength = 0;
  // Read record length
  if (fread(&dataLength, sizeof(dataLength), 1, m_file) != 1){
    err << "getNextTuple:Error reading length  of data part" << endl;
    delete tup;
    res = -1;
    return NULL;
  } // if
  
  // Convert length from network byte order
  dataLength = ntohl(dataLength);
  const Uint32 dataLenBytes = 4 * dataLength;
  
  if (dataLength == 0) {
    // Zero length for last tuple
    // End of this data fragment
    debug << "End of fragment" << endl;
    res = 0;
    delete tup;
    return NULL;
  } // if
  
  tup->createDataRecord(dataLenBytes);
  // Read tuple data
  if (fread(tup->getDataRecord(), 1, dataLenBytes, m_file) != dataLenBytes) {
    err << "getNextTuple:Read error: " << endl;
    delete tup;
    res = -1;
    return NULL;
  }
  
  Uint32 * ptr = tup->getDataRecord();
  ptr += m_currentTable->m_nullBitmaskSize;

  for(int i = 0; i < m_currentTable->m_fixedKeys.size(); i++){
    assert(ptr < tup->getDataRecord() + dataLength);
    
    const Uint32 attrId = m_currentTable->m_fixedKeys[i]->attrId;
    AttributeS * attr = tup->allAttributes[attrId];

    const Uint32 sz = attr->Desc->getSizeInWords();

    attr->Data.null = false;
    attr->Data.void_value = ptr;

    if(!Twiddle(attr))
      {
	res = -1;
	return NULL;
      }
    ptr += sz;
  }

  for(int i = 0; i<m_currentTable->m_fixedAttribs.size(); i++){
    assert(ptr < tup->getDataRecord() + dataLength);

    const Uint32 attrId = m_currentTable->m_fixedAttribs[i]->attrId;
    AttributeS * attr = tup->allAttributes[attrId];

    const Uint32 sz = attr->Desc->getSizeInWords();

    attr->Data.null = false;
    attr->Data.void_value = ptr;

    if(!Twiddle(attr))
      {
	res = -1;
	return NULL;
      }

    ptr += sz;
  }

  for(int i = 0; i<m_currentTable->m_variableAttribs.size(); i++){
    const Uint32 attrId = m_currentTable->m_variableAttribs[i]->attrId;
    AttributeS * attr = tup->allAttributes[attrId];
    
    if(attr->Desc->m_column->getNullable()){
      const Uint32 ind = attr->Desc->m_nullBitIndex;
      if(BitmaskImpl::get(m_currentTable->m_nullBitmaskSize, 
			  tup->getDataRecord(),ind)){
	attr->Data.null = true;
	attr->Data.void_value = NULL;
	continue;
      }
    }

    assert(ptr < tup->getDataRecord() + dataLength);

    typedef BackupFormat::DataFile::VariableData VarData;
    VarData * data = (VarData *)ptr;
    Uint32 sz = ntohl(data->Sz);
    Uint32 id = ntohl(data->Id);
    assert(id == attrId);
    
    attr->Data.null = false;
    attr->Data.void_value = &data->Data[0];

    /**
     * Compute array size
     */
    const Uint32 arraySize = (4 * sz) / (attr->Desc->size / 8);
    assert(arraySize >= attr->Desc->arraySize);
    if(!Twiddle(attr, attr->Desc->arraySize))
      {
	res = -1;
	return NULL;
      }

    ptr += (sz + 2);
  }

  m_count ++;  
  res = 0;
  return tup;
} // RestoreDataIterator::getNextTuple

BackupFile::BackupFile(){
  m_file = 0;
  m_path[0] = 0;
  m_fileName[0] = 0;
  m_buffer = 0;
  m_bufferSize = 0;
}

BackupFile::~BackupFile(){
  if(m_file != 0)
    fclose(m_file);
  if(m_buffer != 0)
    free(m_buffer);
}

bool
BackupFile::openFile(){
  if(m_file != NULL){
    fclose(m_file);
    m_file = 0;
  }
  
  m_file = fopen(m_fileName, "r");
  return m_file != 0;
}

Uint32 *
BackupFile::createBuffer(Uint32 bytes){
  if(bytes > m_bufferSize){
    if(m_buffer != 0)
      free(m_buffer);
    m_bufferSize = m_bufferSize + 2 * bytes;
    m_buffer = (Uint32*)malloc(m_bufferSize);
  }
  return m_buffer;
}

void
BackupFile::setCtlFile(Uint32 nodeId, Uint32 backupId, const char * path){
  m_nodeId = nodeId;
  m_expectedFileHeader.BackupId = backupId;
  m_expectedFileHeader.FileType = BackupFormat::CTL_FILE;

  char name[PATH_MAX]; const Uint32 sz = sizeof(name);
  snprintf(name, sz, "BACKUP-%d.%d.ctl", backupId, nodeId);  
  setName(path, name);
}

void
BackupFile::setDataFile(const BackupFile & bf, Uint32 no){
  m_nodeId = bf.m_nodeId;
  m_expectedFileHeader = bf.m_fileHeader;
  m_expectedFileHeader.FileType = BackupFormat::DATA_FILE;
  
  char name[PATH_MAX]; const Uint32 sz = sizeof(name);
  snprintf(name, sz, "BACKUP-%d-%d.%d.Data", 
	   m_expectedFileHeader.BackupId, no, m_nodeId);
  setName(bf.m_path, name);
}

void
BackupFile::setLogFile(const BackupFile & bf, Uint32 no){
  m_nodeId = bf.m_nodeId;
  m_expectedFileHeader = bf.m_fileHeader;
  m_expectedFileHeader.FileType = BackupFormat::LOG_FILE;
  
  char name[PATH_MAX]; const Uint32 sz = sizeof(name);
  snprintf(name, sz, "BACKUP-%d.%d.log", 
	   m_expectedFileHeader.BackupId, m_nodeId);
  setName(bf.m_path, name);
}

void
BackupFile::setName(const char * p, const char * n){
  const Uint32 sz = sizeof(m_path);
  if(p != 0 && strlen(p) > 0){
    if(p[strlen(p)-1] == '/'){
      snprintf(m_path, sz, "%s", p);
    } else {
      snprintf(m_path, sz, "%s%s", p, "/");
    }
  } else {
    m_path[0] = 0;
  }

  snprintf(m_fileName, sizeof(m_fileName), "%s%s", m_path, n);
  debug << "Filename = " << m_fileName << endl;
}

bool
BackupFile::readHeader(){
  if(!openFile()){
    return false;
  }
  
  if(fread(&m_fileHeader, sizeof(m_fileHeader), 1, m_file) != 1){
    err << "readDataFileHeader: Error reading header" << endl;
    return false;
  }
  
  // Convert from network to host byte order for platform compatibility
  m_fileHeader.NdbVersion  = ntohl(m_fileHeader.NdbVersion);
  m_fileHeader.SectionType = ntohl(m_fileHeader.SectionType);
  m_fileHeader.SectionLength = ntohl(m_fileHeader.SectionLength);
  m_fileHeader.FileType = ntohl(m_fileHeader.FileType);
  m_fileHeader.BackupId = ntohl(m_fileHeader.BackupId);
  m_fileHeader.BackupKey_0 = ntohl(m_fileHeader.BackupKey_0);
  m_fileHeader.BackupKey_1 = ntohl(m_fileHeader.BackupKey_1);

  debug << "FileHeader: " << m_fileHeader.Magic << " " <<
    m_fileHeader.NdbVersion << " " <<
    m_fileHeader.SectionType << " " <<
    m_fileHeader.SectionLength << " " <<
    m_fileHeader.FileType << " " <<
    m_fileHeader.BackupId << " " <<
    m_fileHeader.BackupKey_0 << " " <<
    m_fileHeader.BackupKey_1 << " " <<
    m_fileHeader.ByteOrder << endl;
  
  debug << "ByteOrder is " << m_fileHeader.ByteOrder << endl;
  debug << "magicByteOrder is " << magicByteOrder << endl;
  
  if (m_fileHeader.FileType != m_expectedFileHeader.FileType){
    abort();
  }
  
  // Check for BackupFormat::FileHeader::ByteOrder if swapping is needed
  if (m_fileHeader.ByteOrder == magicByteOrder) {
    m_hostByteOrder = true;
  } else if (m_fileHeader.ByteOrder == swappedMagicByteOrder){
    m_hostByteOrder = false;
  } else {
    abort();
  }
  
  return true;
} // BackupFile::readHeader

bool
BackupFile::validateFooter(){
  return true;
}

bool
RestoreDataIterator::readFragmentHeader(int & ret)
{
  BackupFormat::DataFile::FragmentHeader Header;
  
  debug << "RestoreDataIterator::getNextFragment" << endl;
  
  if (fread(&Header, sizeof(Header), 1, m_file) != 1){
    ret = 0;
    return false;
  } // if
  
  Header.SectionType  = ntohl(Header.SectionType);
  Header.SectionLength  = ntohl(Header.SectionLength);
  Header.TableId  = ntohl(Header.TableId);
  Header.FragmentNo  = ntohl(Header.FragmentNo);
  Header.ChecksumType  = ntohl(Header.ChecksumType);
  
  debug << "FragmentHeader: " << Header.SectionType 
	<< " " << Header.SectionLength 
	<< " " << Header.TableId 
	<< " " << Header.FragmentNo 
	<< " " << Header.ChecksumType << endl;
  
  m_currentTable = m_metaData.getTable(Header.TableId);
  if(m_currentTable == 0){
    ret = -1;
    return false;
  }
  
  info << "_____________________________________________________" << endl
       << "Restoring data in table: " << m_currentTable->getTableName() 
       << "(" << Header.TableId << ") fragment " 
       << Header.FragmentNo << endl;
  
  m_count = 0;
  ret = 0;
  return true;
} // RestoreDataIterator::getNextFragment


bool
RestoreDataIterator::validateFragmentFooter() {
  BackupFormat::DataFile::FragmentFooter footer;
  
  if (fread(&footer, sizeof(footer), 1, m_file) != 1){
    err << "getFragmentFooter:Error reading fragment footer" << endl;
    return false;
  } 
  
  // TODO: Handle footer, nothing yet
  footer.SectionType  = ntohl(footer.SectionType);
  footer.SectionLength  = ntohl(footer.SectionLength);
  footer.TableId  = ntohl(footer.TableId);
  footer.FragmentNo  = ntohl(footer.FragmentNo);
  footer.NoOfRecords  = ntohl(footer.NoOfRecords);
  footer.Checksum  = ntohl(footer.Checksum);

  assert(m_count == footer.NoOfRecords);
  
  return true;
} // RestoreDataIterator::getFragmentFooter

AttributeDesc::AttributeDesc(NdbDictionary::Column *c)
  : m_column(c)
{
  size = c->getSize()*8;
  arraySize = c->getLength();
}

void TableS::createAttr(NdbDictionary::Column *column)
{
  AttributeDesc * d = new AttributeDesc(column);
  if(d == NULL) {
    ndbout_c("Restore: Failed to allocate memory");
    abort();
  }
  d->attrId = allAttributesDesc.size();
  allAttributesDesc.push_back(d);

  if(d->m_column->getPrimaryKey() /* && not variable */)
  {
    m_fixedKeys.push_back(d);
    return;
  }

  if(!d->m_column->getNullable())
  {
    m_fixedAttribs.push_back(d);
    return;
  }

  /* Nullable attr*/
  d->m_nullBitIndex = m_noOfNullable; 
  m_noOfNullable++;
  m_nullBitmaskSize = (m_noOfNullable + 31) / 32;
  m_variableAttribs.push_back(d);
} // TableS::createAttr

Uint16 Twiddle16(Uint16 in)
{
  Uint16 retVal = 0;

  retVal = ((in & 0xFF00) >> 8) |
    ((in & 0x00FF) << 8);

  return(retVal);
} // Twiddle16

Uint32 Twiddle32(Uint32 in)
{
  Uint32 retVal = 0;

  retVal = ((in & 0x000000FF) << 24) | 
    ((in & 0x0000FF00) << 8)  |
    ((in & 0x00FF0000) >> 8)  |
    ((in & 0xFF000000) >> 24);
  
  return(retVal);
} // Twiddle32

Uint64 Twiddle64(Uint64 in)
{
  Uint64 retVal = 0;

  retVal = 
    ((in & (Uint64)0x00000000000000FFLL) << 56) | 
    ((in & (Uint64)0x000000000000FF00LL) << 40) | 
    ((in & (Uint64)0x0000000000FF0000LL) << 24) | 
    ((in & (Uint64)0x00000000FF000000LL) << 8) | 
    ((in & (Uint64)0x000000FF00000000LL) >> 8) | 
    ((in & (Uint64)0x0000FF0000000000LL) >> 24) | 
    ((in & (Uint64)0x00FF000000000000LL) >> 40) | 
    ((in & (Uint64)0xFF00000000000000LL) >> 56);

  return(retVal);
} // Twiddle64


RestoreLogIterator::RestoreLogIterator(const RestoreMetaData & md)
  : m_metaData(md) 
{
  debug << "RestoreLog constructor" << endl;
  setLogFile(md, 0);

  m_count = 0;
}

const LogEntry *
RestoreLogIterator::getNextLogEntry(int & res) {
  // Read record length
  typedef BackupFormat::LogFile::LogEntry LogE;

  Uint32 gcp = 0;
  LogE * logE = 0;
  Uint32 len = ~0;
  const Uint32 stopGCP = m_metaData.getStopGCP();
  do {
    
    if(createBuffer(4) == 0) {
      res = -1;
      return NULL;
    }
     

    if (fread(m_buffer, sizeof(Uint32), 1, m_file) != 1){
      res = -1;
      return NULL;
    }
    
    m_buffer[0] = ntohl(m_buffer[0]);
    len = m_buffer[0];
    if(len == 0){
      res = 0;
      return 0;
    }

    if(createBuffer(4 * (len + 1)) == 0){
      res = -1;
      return NULL;
    }
    
    if (fread(&m_buffer[1], 4, len, m_file) != len) {
      res = -1;
      return NULL;
    }
    
    logE = (LogE *)&m_buffer[0];
    logE->TableId = ntohl(logE->TableId);
    logE->TriggerEvent = ntohl(logE->TriggerEvent);
    
    const bool hasGcp = (logE->TriggerEvent & 0x10000) != 0;
    logE->TriggerEvent &= 0xFFFF;
    
    if(hasGcp){
      len--;
      gcp = ntohl(logE->Data[len-2]);
    }
  } while(gcp > stopGCP + 1);

  for(int i=0; i<m_logEntry.m_values.size();i++)
    delete m_logEntry.m_values[i];
  m_logEntry.m_values.clear();
  m_logEntry.m_table = m_metaData.getTable(logE->TableId);
  switch(logE->TriggerEvent){
  case TriggerEvent::TE_INSERT:
    m_logEntry.m_type = LogEntry::LE_INSERT;
    break;
  case TriggerEvent::TE_UPDATE:
    m_logEntry.m_type = LogEntry::LE_UPDATE;
    break;
  case TriggerEvent::TE_DELETE:
    m_logEntry.m_type = LogEntry::LE_DELETE;
    break;
  default:
    res = -1;
    return NULL;
  }

  const TableS * tab = m_logEntry.m_table;

  AttributeHeader * ah = (AttributeHeader *)&logE->Data[0];
  AttributeHeader *end = (AttributeHeader *)&logE->Data[len - 2];
  AttributeS *  attr;
  while(ah < end){
    attr = new AttributeS;
    if(attr == NULL) {
      ndbout_c("Restore: Failed to allocate memory");
      res = -1;
      return NULL;
    }
    attr->Desc = (* tab)[ah->getAttributeId()];
    assert(attr->Desc != 0);

    const Uint32 sz = ah->getDataSize();
    if(sz == 0){
      attr->Data.null = true;
      attr->Data.void_value = NULL;
    } else {
      attr->Data.null = false;
      attr->Data.void_value = ah->getDataPtr();
    }
    
    Twiddle(attr);
    m_logEntry.m_values.push_back(attr);
    
    ah = ah->getNext();
  }
  
  m_count ++;
  res = 0;
  return &m_logEntry;
}
