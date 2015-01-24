/**
 * @file   loader.cc
 * @author Stavros Papadopoulos <stavrosp@csail.mit.edu>
 *
 * @section LICENSE
 *
 * The MIT License
 * 
 * @copyright Copyright (c) 2014 Stavros Papadopoulos <stavrosp@csail.mit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * 
 * @section DESCRIPTION
 *
 * This file implements the Loader class.
 */

#include "loader.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <iostream>

/******************************************************
********************* CONSTRUCTORS ********************
******************************************************/

Loader::Loader(const std::string& workspace, StorageManager& storage_manager) 
    : storage_manager_(storage_manager) {
  set_workspace(workspace);
  create_workspace(); 
}

/******************************************************
******************* LOADING FUNCTIONS *****************
******************************************************/

void Loader::load(const std::string& filename, 
                  const ArraySchema& array_schema) const {
  // Open array in CREATE mode
  StorageManager::ArrayDescriptor* ad = 
      storage_manager_.open_array(array_schema);

  // Prepare filenames
  std::string to_be_sorted_filename = filename;
  if(to_be_sorted_filename[0] == '~') 
    to_be_sorted_filename = std::string(getenv("HOME")) +
        to_be_sorted_filename.substr(1, workspace_.size()-1);
  assert(check_on_load(to_be_sorted_filename));
  std::string sorted_filename = workspace_ + "/sorted_" +
                                array_schema.array_name() + ".csv";
  std::string injected_filename("");
  
  // For easy reference
  bool regular = array_schema.has_regular_tiles();
  ArraySchema::Order order = array_schema.order();
  
  // Inject ids if needed
  if(regular || order == ArraySchema::HILBERT) {
    injected_filename = workspace_ + "/injected_" +
                        array_schema.array_name() + ".csv";
    try {
      inject_ids_to_csv_file(filename, injected_filename, array_schema); 
    } catch(LoaderException& le) {
      remove(injected_filename.c_str());
      storage_manager_.delete_array(array_schema.array_name());
      throw LoaderException("[Loader] Cannot load CSV file '" + filename + 
                            "'.\n " + le.what());
    }
    to_be_sorted_filename = injected_filename;
  }

  // Sort CSV
  sort_csv_file(to_be_sorted_filename, sorted_filename, array_schema);
  remove(injected_filename.c_str());

  // Make tiles
  try {
    if(regular)
      make_tiles_regular(sorted_filename, ad, array_schema);
    else
      make_tiles_irregular(sorted_filename, ad, array_schema);
  } catch(LoaderException& le) {
    remove(sorted_filename.c_str());
    storage_manager_.delete_array(array_schema.array_name());
    throw LoaderException("[Loader] Cannot load CSV file '" + filename + 
                          "'.\n " + le.what());
  } 

  // Clean up and close array 
  remove(sorted_filename.c_str());
  storage_manager_.close_array(ad);
}

void Loader::load_CSV_gVCF(const std::string& filename, const char* array_name, const uint64_t max_sample_idx) const {
  // Create gVCF array schema
  ArraySchema* array_schema = create_gVCF_array_schema(array_name, max_sample_idx);

  // Open array in CREATE mode
  StorageManager::ArrayDescriptor* ad = 
      storage_manager_.open_array(*array_schema);

  // Prepare filenames
  std::string to_be_sorted_filename = filename;
  if(to_be_sorted_filename[0] == '~') 
    to_be_sorted_filename = std::string(getenv("HOME")) +
        to_be_sorted_filename.substr(1, workspace_.size()-1);
  assert(check_on_load(to_be_sorted_filename));
  std::string sorted_filename = workspace_ + "/sorted_" +
                                array_schema->array_name() + ".csv";

  // Sort CSV
  sort_csv_file(to_be_sorted_filename, sorted_filename, *array_schema);

  // Make tiles
  try {
    make_tiles_irregular_CSV_gVCF(sorted_filename, ad, *array_schema);
  } catch(LoaderException& le) {
    remove(sorted_filename.c_str());
    storage_manager_.delete_array(array_schema->array_name());
    throw LoaderException("[Loader] Cannot load CSV file '" + filename + 
                          "'.\n " + le.what());
  } 

  // Clean up and close array 
  remove(sorted_filename.c_str());
  storage_manager_.close_array(ad);
  delete array_schema;
}


/******************************************************
******************* PRIVATE METHODS *******************
******************************************************/

inline
void Loader::append_cell(const ArraySchema& array_schema, 
                         CSVLine* csv_line, Tile** tiles) const {
  // For easy reference
  unsigned int attribute_num = array_schema.attribute_num();

  // Append coordinates first
  if(!(*csv_line >> *tiles[attribute_num]))
    throw LoaderException("[Append cell] Cannot read coordinates "
                          "from CSV file.");
  // Append attribute values
  for(unsigned int i=0; i<attribute_num; i++)
    if(!(*csv_line >> *tiles[i]))
      throw LoaderException("[Append cell] Cannot read attribute "
                            "value from CSV file.");
}

void Loader::append_cell_gVCF(const ArraySchema& array_schema, 
                              CSVLine* csv_line, Tile** tiles) const {
  // For easy reference
  unsigned int attribute_num = array_schema.attribute_num();

  // Bitmap that indicates the NULL attribute values
  int NULL_bitmap = 0;  
 
  // Variables for retrieving the CSV values
  int int_v;
  int64_t int64_t_v;
  float float_v;
  std::string string_v;

  // Offsets
  int64_t REF_offset = tiles[1]->cell_num(); 
  int64_t ALT_offset = tiles[2]->cell_num(); 
  int64_t FILTER_ID_offset = tiles[4]->cell_num(); 
  int64_t AD_offset = tiles[19]->cell_num(); 
  int64_t PL_offset = tiles[20]->cell_num(); 
  
  // ALT_num and FILTER_num variables
  int ALT_num;
  int FILTER_num;

  // Append coordinates first
  if(!(*csv_line >> *tiles[attribute_num]))
    throw LoaderException("Cannot read coordinates from CSV file.");

  // END -- Attribute 0
  if(!(*csv_line >> *tiles[0]))
    throw LoaderException("Cannot read END value from CSV file.");

  // REF -- Attribute 1
  if(!(*csv_line >> string_v))
    throw LoaderException("Cannot read REF value from CSV file.");
  for(int i=0; i<string_v.size(); i++)
    *tiles[1] << string_v[i];
  *tiles[1] << '\0';

  // ALT -- Attribute 2
  if(!(*csv_line >> ALT_num))
    throw LoaderException("Cannot read ALT_num value from CSV file.");
  for(int j=0; j<ALT_num; j++) {
    if(!(*csv_line >> string_v))
      throw LoaderException("Cannot read ALT value from CSV file.");
    for(int i=0; i<string_v.size(); i++)
      *tiles[2] << string_v[i];
    if(j < ALT_num-1) // Do not store '\0' after last allele '&' for <NON_REF>
      *tiles[2] << '\0';
    else
      assert(string_v == "&");
  }
  
  // QUAL -- Attribute 3
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read QUAL value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[3] << 0;
  } else {  
    *tiles[3] << float_v;
  }
  NULL_bitmap << 1; 

  // FILTER_ID -- Attribute 4
  if(!(*csv_line >> FILTER_num))
    throw LoaderException("Cannot read FILTER_num value from CSV file.");
  *tiles[4] << FILTER_num;  
  for(int j=0; j<FILTER_num; j++) {
    if(!(*csv_line >> *tiles[4]))
      throw LoaderException("Cannot read FILTER_ID value from CSV file.");
  }

  // BaseQRankSum -- Attribute 5
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read BaseQRankSum value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[5] << 0;
  } else {
    *tiles[5] << float_v;
  }
  NULL_bitmap << 1; 

  // ClippingRankSum -- Attribute 6
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read ClippingSum value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[6] << 0;
  } else {
    *tiles[6] << float_v;
  }
  NULL_bitmap << 1; 

  // MQRankSum -- Attribute 7
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read MQRankSum value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[7] << 0;
  } else {
    *tiles[7] << float_v;
  }
  NULL_bitmap << 1; 

  // ReadPosRankSum -- Attribute 8
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read ReadPosRankSum value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[8] << 0;
  } else {
    *tiles[8] << float_v;
  }
  NULL_bitmap << 1; 

  // DP -- Attribute 9
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read DP value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[9] << 0;
  } else {
    *tiles[9] << int_v;
  }
  NULL_bitmap << 1; 

  // MQ -- Attribute 10
  if(!(*csv_line >> float_v))
    throw LoaderException("Cannot read MQ value from CSV file.");
  if(float_v == CSV_NULL_FLOAT) {
    NULL_bitmap += 1;
    *tiles[10] << 0;
  } else {
    *tiles[10] << float_v;
  }
  NULL_bitmap << 1; 

  // MQ0 -- Attribute 11
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read MQ0 value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[11] << 0;
  } else {
    *tiles[11] << int_v;
  }
  NULL_bitmap << 1; 

  // DP_FMT -- Attribute 12
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read DP_FMT value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[12] << 0;
  } else {
    *tiles[12] << int_v;
  }
  NULL_bitmap << 1; 

  // MIN_DP -- Attribute 13
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read MIN_DP value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[13] << 0;
  } else {
    *tiles[13] << int_v;
  }
  NULL_bitmap << 1; 

  // GQ -- Attribute 14
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read GQ value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[14] << 0;
  } else {
    *tiles[14] << int_v;
  }
  NULL_bitmap << 1; 

  // SB_1 -- Attribute 15
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read SB_1 value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[15] << 0;
  } else {
    *tiles[15] << int_v;
  }
  NULL_bitmap << 1; 

  // SB_2 -- Attribute 16
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read SB_2 value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[16] << 0;
  } else {
    *tiles[16] << int_v;
  }
  NULL_bitmap << 1; 

  // SB_3 -- Attribute 17
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read SB_3 value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[17] << 0;
  } else {
    *tiles[17] << int_v;
  }
  NULL_bitmap << 1; 

  // SB_4 -- Attribute 18
  if(!(*csv_line >> int_v))
    throw LoaderException("Cannot read SB_4 value from CSV file.");
  if(int_v == CSV_NULL_INT) {
    NULL_bitmap += 1;
    *tiles[18] << 0;
  } else {
    *tiles[18] << int_v;
  }
  NULL_bitmap << 1;

  // AD -- Attribute 19
  for(int i=0; i<ALT_num+1; i++) {
    if(!(*csv_line >> int_v))
      throw LoaderException("Cannot read AD value from CSV file.");
    if(int_v == CSV_NULL_INT)
      *tiles[19] << 0;
    else 
      *tiles[19] << int_v;
  }
  if(int_v == CSV_NULL_INT) // We assume that if one AD value is NULL, all are
    NULL_bitmap += 1;
  NULL_bitmap << 1;
  
  // PL -- Attribute 20
  for(int i=0; i<(ALT_num+1)*(ALT_num+2)/2; i++) {
    if(!(*csv_line >> int_v))
      throw LoaderException("Cannot read PL value from CSV file.");
    if(int_v == CSV_NULL_INT)
      *tiles[20] << 0;
    else 
      *tiles[20] << int_v;
  }
  if(int_v == CSV_NULL_INT) // We assume that if one PL value is NULL, all are
    NULL_bitmap += 1;

  // NULL -- Attribute 21
  *tiles[21] << NULL_bitmap;

  // OFFSETS -- Attribute 22
  *tiles[22] << REF_offset;
  *tiles[22] << ALT_offset;
  *tiles[22] << FILTER_ID_offset;
  *tiles[22] << AD_offset;
  *tiles[22] << PL_offset;
}

bool Loader::check_on_load(const std::string& filename) const {
  int fd = open(filename.c_str(), O_RDONLY);
  if(fd == -1)
    return false; 
   
  close(fd);

  return true;
}

ArraySchema* Loader::create_gVCF_array_schema(const char* array_name, const uint64_t max_sample_idx) const {
  // Set dimension names
  std::vector<std::string> dim_names;
  dim_names.push_back("SampleID"); 
  dim_names.push_back("POS"); 
  
  // Set dimension domains
  std::vector<std::pair<double,double> > dim_domains;
  dim_domains.push_back(std::pair<double,double>(0, max_sample_idx));
  dim_domains.push_back(std::pair<double,double>(0, 6000000000));

  // Set attribute names
  std::vector<std::string> attribute_names;
  attribute_names.push_back("END"); 
  attribute_names.push_back("REF"); 
  attribute_names.push_back("ALT"); 
  attribute_names.push_back("QUAL"); 
  attribute_names.push_back("FILTER_ID"); 
  attribute_names.push_back("BaseQRankSum"); 
  attribute_names.push_back("ClippingRankSum"); 
  attribute_names.push_back("MQRankSum"); 
  attribute_names.push_back("ReadPosRankSum"); 
  attribute_names.push_back("DP"); 
  attribute_names.push_back("MQ"); 
  attribute_names.push_back("MQ0"); 
  attribute_names.push_back("DP_FMT"); 
  attribute_names.push_back("MIN_DP"); 
  attribute_names.push_back("GQ"); 
  attribute_names.push_back("SB_1"); 
  attribute_names.push_back("SB_2"); 
  attribute_names.push_back("SB_3"); 
  attribute_names.push_back("SB_4"); 
  attribute_names.push_back("AD"); 
  attribute_names.push_back("PL"); 
  attribute_names.push_back("NULL"); 
  attribute_names.push_back("OFFSETS"); 
 
  // Set attribute types. 
  // The first types are for the attributes, whereas the very last type is 
  // for all the dimensions collectively. 
  std::vector<const std::type_info*> types;
  types.push_back(&typeid(int64_t));  // END
  types.push_back(&typeid(char));     // REF 
  types.push_back(&typeid(char));     // ALT
  types.push_back(&typeid(float));    // QUAL
  types.push_back(&typeid(int));      // FILTER_ID
  types.push_back(&typeid(float));    // BaseQRankSum
  types.push_back(&typeid(float));    // ClippingRankSum
  types.push_back(&typeid(float));    // MQRankSum
  types.push_back(&typeid(float));    // ReadPosRankSum
  types.push_back(&typeid(int));      // DP
  types.push_back(&typeid(float));    // MQ
  types.push_back(&typeid(int));      // MQ0
  types.push_back(&typeid(int));      // DP_FMT
  types.push_back(&typeid(int));      // MIN_DP
  types.push_back(&typeid(int));      // GQ
  types.push_back(&typeid(int));      // SB_1
  types.push_back(&typeid(int));      // SB_2
  types.push_back(&typeid(int));      // SB_3
  types.push_back(&typeid(int));      // SB_4
  types.push_back(&typeid(int));      // AD
  types.push_back(&typeid(int));      // PL
  types.push_back(&typeid(int));      // NULL
  types.push_back(&typeid(int64_t));  // OFFSETS 
  types.push_back(&typeid(int64_t));  // Coordinates (SampleID, POS)

  // Set order and capacity
  ArraySchema::Order order = ArraySchema::COLUMN_MAJOR;
  uint64_t capacity = 1000;

  // Return schema with regular tiles
  return new ArraySchema(array_name, attribute_names, dim_names, dim_domains, 
                         types, order, capacity);
}

void Loader::create_workspace() const {
  struct stat st;
  stat(workspace_.c_str(), &st);

  // If the workspace does not exist, create it
  if(!S_ISDIR(st.st_mode)) { 
    int dir_flag = mkdir(workspace_.c_str(), S_IRWXU);
    assert(dir_flag == 0);
  }
}

void Loader::inject_ids_to_csv_file(const std::string& filename,
                                    const std::string& injected_filename,
                                    const ArraySchema& array_schema) const {
  assert(array_schema.has_regular_tiles() || 
         array_schema.order() == ArraySchema::HILBERT);

  // For easy reference
  unsigned int dim_num = array_schema.dim_num();
  ArraySchema::Order order = array_schema.order();

  // Initialization
  CSVFile csv_file_in(filename, CSVFile::READ);
  CSVFile csv_file_out(injected_filename, CSVFile::WRITE);
  CSVLine line_in, line_out;
  std::vector<double> coordinates;
  coordinates.resize(dim_num);
  double coordinate;

  while(csv_file_in >> line_in) {
    // Retrieve coordinates from the input line
    for(unsigned int i=0; i<dim_num; i++) {
      if(!(line_in >> coordinate))
        throw LoaderException("[Inject ids] Cannot read coordinate "
                              "value from CSV file."); 
      coordinates[i] = coordinate;
    }
    // Put the id at the beginning of the output line
    if(array_schema.has_regular_tiles()) { // Regular tiles
      if(order == ArraySchema::HILBERT)
        line_out = array_schema.tile_id_hilbert(coordinates);
      else if(order == ArraySchema::ROW_MAJOR)
        line_out = array_schema.tile_id_row_major(coordinates);
      else if(order == ArraySchema::COLUMN_MAJOR)
        line_out = array_schema.tile_id_column_major(coordinates);
    } else { // Irregular tiles + Hilbert cell order
        line_out = array_schema.cell_id_hilbert(coordinates);
    }
    
    // Append the input line to the output line, 
    // and then into the output CSV file
    line_out << line_in;
    csv_file_out << line_out;
  }
}

void Loader::make_tiles_irregular(const std::string& filename,
                                  const StorageManager::ArrayDescriptor* ad, 
                                  const ArraySchema& array_schema) const {
  // For easy reference
  ArraySchema::Order order = array_schema.order();
  uint64_t capacity = array_schema.capacity();
 
  // Initialization 
  CSVFile csv_file(filename, CSVFile::READ);
  CSVLine csv_line;
  Tile** tiles = new Tile*[array_schema.attribute_num() + 1]; 
  uint64_t tile_id = 0;
  uint64_t cell_id;
  uint64_t cell_num = 0;

  new_tiles(array_schema, tile_id, tiles);

  while(csv_file >> csv_line) {
    if(cell_num == capacity) {
      store_tiles(ad, tiles);
      new_tiles(array_schema, ++tile_id, tiles);
      cell_num = 0;
    }
    // Skip the Hilbert id
    if(order == ArraySchema::HILBERT)
      if(!(csv_line >> cell_id)) {
        delete [] tiles;
        throw LoaderException("[Make tiles] Cannot read cell id."); 
      } 
   
    try {
      // Every CSV line is a logical cell
      append_cell(array_schema, &csv_line, tiles);     
    } catch(LoaderException& le) {
      delete [] tiles;
      throw LoaderException("[Make tiles] " + le.what()); 
    }
    cell_num++;
  }

  // Store the lastly created tiles
  store_tiles(ad, tiles);

  delete [] tiles; 
}

void Loader::make_tiles_irregular_CSV_gVCF(
    const std::string& filename,
    const StorageManager::ArrayDescriptor* ad, 
    const ArraySchema& array_schema) const {
  // For easy reference
  ArraySchema::Order order = array_schema.order();
  uint64_t capacity = array_schema.capacity();
 
  // Initialization 
  CSVFile csv_file(filename, CSVFile::READ);
  CSVLine csv_line;
  Tile** tiles = new Tile*[array_schema.attribute_num() + 1]; 
  uint64_t tile_id = 0;
  uint64_t cell_num = 0;

  new_tiles(array_schema, tile_id, tiles);

  while(csv_file >> csv_line) {
    if(cell_num == capacity) {
      store_tiles(ad, tiles);
      new_tiles(array_schema, ++tile_id, tiles);
      cell_num = 0;
    }
   
    try {
      // Every CSV line is a logical cell
      append_cell_gVCF(array_schema, &csv_line, tiles);     
    } catch(LoaderException& le) {
      delete [] tiles;
      throw LoaderException("[Make tiles] " + le.what()); 
    }
    cell_num++;
  }

  // Store the lastly created tiles
  store_tiles(ad, tiles);

  delete [] tiles; 
}

// Note: The array must be open in CREATE mode
void Loader::make_tiles_regular(const std::string& filename, 
                                const StorageManager::ArrayDescriptor* ad, 
                                const ArraySchema& array_schema) const {
  CSVFile csv_file(filename, CSVFile::READ);
  CSVLine csv_line;
  Tile** tiles = new Tile*[array_schema.attribute_num() + 1]; 
  uint64_t previous_tile_id = LD_INVALID_TILE_ID, tile_id;
  
  // Proecess the following lines 
  while(csv_file >> csv_line) {
    if(!(csv_line >> tile_id)) {
      delete [] tiles;
      throw LoaderException("[Make tiles] Cannot read tile id."); 
    }
    if(tile_id != previous_tile_id) {
      // Send the tiles to the storage manager and initialize new ones
      if(previous_tile_id != LD_INVALID_TILE_ID) 
        store_tiles(ad, tiles);
      new_tiles(array_schema, tile_id, tiles);
      previous_tile_id = tile_id;
    }

    try{
      // Every CSV line is a logical cell
      append_cell(array_schema, &csv_line, tiles);
    } catch(LoaderException& le) {
      delete [] tiles;
      throw LoaderException("[Make tiles] " + le.what()) ; 
    }
  }

  // Store the lastly created tiles
  if(previous_tile_id != LD_INVALID_TILE_ID)
    store_tiles(ad, tiles);

  delete [] tiles; 
}

inline
void Loader::new_tiles(const ArraySchema& array_schema, 
                       uint64_t tile_id, Tile** tiles) const {
  // For easy reference
  unsigned int attribute_num = array_schema.attribute_num();
  uint64_t capacity = array_schema.capacity();

  for(unsigned int i=0; i<=attribute_num; i++)
    tiles[i] = storage_manager_.new_tile(array_schema, i, tile_id, capacity);
}


bool Loader::path_exists(const std::string& path) const {
  struct stat st;
  stat(path.c_str(), &st);
  return S_ISDIR(st.st_mode);
}

inline
void Loader::set_workspace(const std::string& path) {
  workspace_ = path;
  
  // Replace '~' with the absolute path
  if(workspace_[0] == '~') {
    workspace_ = std::string(getenv("HOME")) +
                 workspace_.substr(1, workspace_.size()-1);
  }

  // Check if the input path is an existing directory 
  assert(path_exists(workspace_));
 
  workspace_ += "/Loader";
}

void Loader::sort_csv_file(const std::string& to_be_sorted_filename, 
                           const std::string& sorted_filename,    
                           const ArraySchema& array_schema) const {
  // Prepare Linux sort command
  char sub_cmd[50];
  std::string cmd;

  cmd = "sort -t, ";
  
  // For easy reference
  unsigned int dim_num = array_schema.dim_num();
  bool regular = array_schema.has_regular_tiles();
  ArraySchema::Order order = array_schema.order();

  if(regular || order == ArraySchema::HILBERT) { 
    // to_be_sorted_filename line format:  
    // [tile_id or hilbert_cell_id],dim#1,dim#2,...,attr#1,attr#2,...
    // The tile order is determined by tile_id for regular tiles,
    // or hilbert_cell_id for irregular tiles.
    // Ties are broken by default by sorting according to ROW_MAJOR.
    cmd += "-k1,1n ";
    for(unsigned int i=2; i<dim_num+2; i++) {
      sprintf(sub_cmd, "-k%u,%un ", i, i);
      cmd += sub_cmd;
    }
  } else { // Irregular tiles + [ROW_MAJOR or COLUMN_MAJOR]
    // to_be_sorted_filename line format:  
    // dim#1,dim#2,...,attr#1,attr#2,...
    for(unsigned int i=1; i<dim_num+1; i++) {
      if(order == ArraySchema::ROW_MAJOR)
        sprintf(sub_cmd, "-k%u,%un ", i, i);
      else if (order == ArraySchema::COLUMN_MAJOR) 
        sprintf(sub_cmd, "-k%u,%un ", dim_num+1-i, dim_num+1-i);
      else // Unsupported order
        assert(0);
      cmd += sub_cmd;
    }
  }
  cmd += to_be_sorted_filename + " > " + sorted_filename;

  // Invoke Linux sort command
  system(cmd.c_str());
}

inline
void Loader::store_tiles(const StorageManager::ArrayDescriptor* ad,
                         Tile** tiles) const {
  // For easy reference
  const ArraySchema& array_schema = ad->array_schema(); 
  unsigned int attribute_num = array_schema.attribute_num();

  // Append attribute tiles
  for(unsigned int i=0; i<=attribute_num; i++)
    storage_manager_.append_tile(tiles[i], ad, i);
}

