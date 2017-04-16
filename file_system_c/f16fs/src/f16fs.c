#include "f16fs.h"
#include "block_store.h"
#include <stdio.h>
#include <math.h>


typedef struct {
	uint8_t file_type;
	uint8_t use_flag;	//0 for unused, 1 for used
	unsigned long file_size;
	int num_blocks_in_use;
	uint8_t padding[21];
	uint16_t direct_block_ptr_array[6];	//will simply be block_store block_id
	uint16_t indirect_block_ptr;
	uint16_t double_indirect_block_ptr;
} inode_t;

typedef struct {
	int inode_index;
	unsigned long offset;
} file_descriptor_t;

struct F16FS {
	block_store_t *fs;
	file_descriptor_t file_descriptors[256];
	inode_t inodes[256];
	int total_files;
};

typedef struct{
	file_record_t records[7];
	uint8_t padding[5];
	uint8_t num_entries;
}directory_t;

//traverses a directory path to return the parent inode of the ultimate destination
//\takes a F16FS file system struct and a dyn_array of parsed path tokens
//\returns an inode_t struct or NULL on error
inode_t* directory_traversal(F16FS_t* fs, dyn_array_t* tokens);

//simple helper function to free all the memory mallocs from directory traversal to keep the code cleaner
void directory_traversal_frees(directory_t *root_directory, directory_t *working_directory, inode_t *root_inode);

//gets a block number ("block ptr") from block store in file system, allocates a block if neccessary
//\takes: F16FS_t file ssytem struct
//\an inode index that is used to index into files systems inode table
//\a block number to start at based on where we want to start writing or read in a file
//\a flag that indicates if this is being called by read or write so we can differentiate when to allocate blocks while writing
//\returns a valid block number on success, -1 on error
int get_block_ptr(F16FS_t* fs, int inode_index_for_write, int block_to_start_at, uint8_t read_write_flag);



F16FS_t *fs_format(const char *path){

	//parameter validation
	if(path == NULL || strcmp(path, "") == 0){
		return NULL;
	}

	int i, j = 0;
	unsigned blockid;

	F16FS_t *f16fs = (F16FS_t*) calloc(1, sizeof(F16FS_t));

	//make sure calloc didn't fail
	if(f16fs == NULL){
		return NULL;
	}

	f16fs->fs = block_store_create(path);

	//format inodes on filesystem
	//each inode is 64 bytes; there will be 32 data blocks worth of 
	//inodes(*512 byte data block size)so the inode table will be 16kb = 256 actual inodes
	//and 8 inodes per block so use an array of 8 inodes to stamp that structure into each
	//of the 32 blocks of inodes
	inode_t inodes[8] = {{0}};

	//set up root inode
	inodes[0].file_type = FS_DIRECTORY;
	inodes[0].file_size = sizeof(directory_t);
	inodes[0].use_flag = 1;
	inodes[0].direct_block_ptr_array[0] = 48;

	//root inode now lives at blockid 16 and has a direct block pointer pointing to blockid 48, the location of root directory
	blockid = block_store_allocate(f16fs->fs);
	block_store_write(f16fs->fs, blockid, inodes);
	
	//reset to 0 before looping to initialize the rest of our inodes
	inodes[0].file_type = 0;
	inodes[0].file_size = 0;
	inodes[0].use_flag = 0;
	inodes[0].direct_block_ptr_array[0] = 0;

	//initialize the other 31 inodes
	for(i = 1; i < 32; i++){
		blockid = block_store_allocate(f16fs->fs);
		block_store_write(f16fs->fs, blockid, inodes);
	}

	//initialize file descriptors to invalid state
	for(i = 0; i < 256; i++){
		f16fs->file_descriptors[i].inode_index = -1;
	}
	
	//initialize root directory
	blockid = block_store_allocate(f16fs->fs);
	directory_t *root = (directory_t*) calloc(1,sizeof(directory_t));

	block_store_write(f16fs->fs, blockid, root);

	//store inodeTable in f16fs struct
	for(i = 0; i < 32; i++){	//32 512 byte blocks of inodes
		block_store_read(f16fs->fs, i + 16, inodes);
		memcpy(&(f16fs->inodes[j]), inodes, sizeof(inodes));
		j += 8;
	}


	free(root);

	return f16fs;
}


F16FS_t *fs_mount(const char *path){

	inode_t inodes[8];
	int i, j = 0;

	//parameter validation
	if(path == NULL || strcmp(path, "") == 0){
		return NULL;
	}

	//allocate an F16FS_t and set it's fs pointer to point to a newly opened block_store object
	F16FS_t *f16fs = (F16FS_t*) calloc(1, sizeof(F16FS_t));

	//open a block store and check that it opened correctly
	if(!(f16fs->fs = block_store_open(path))){
		free(f16fs);
		return NULL;
	}

	//initialize file descriptors to invalid state
	for(i = 0; i < 256; i++){
		f16fs->file_descriptors[i].inode_index = -1;
	}

	//store inodeTable in f16fs struct
	for(i = 0; i < 32; i++){	//32 512 byte blocks of inodes
		block_store_read(f16fs->fs, i + 16, inodes);
		memcpy(&(f16fs->inodes[j]), inodes, sizeof(inodes));
		j += 8;
	}

	return f16fs;
}

int fs_unmount(F16FS_t *fs){

	//parameter validation
	if(fs == NULL){
		return -1;
	}

	inode_t inodes[8];
	int i, j = 0;

	//write our modified inodeTable back to the storage device
	for(i = 0; i < 32; i++){	//32 512 byte blocks of inodes
		memcpy(inodes, &(fs->inodes[j]), sizeof(inodes));
		block_store_write(fs->fs, i + 16, inodes);
		
		j += 8;
	}

	//free the block store from memory
	if(fs->fs != NULL){
		block_store_close(fs->fs);
		free(fs);
		return 0;
	}

	return -1;
}

//helper function that crawls through a directory tree and returns an inode for the parent of end of path inode, returns NULL on error
inode_t* directory_traversal(F16FS_t *fs, dyn_array_t* tokens){
	
	if(fs == NULL || tokens == NULL){
		return NULL;
	}

	//parse the path into a list of ordered tokens
	int num_elements = dyn_array_size(tokens);
	int i, j = 0;



	directory_t *root_directory = (directory_t*) calloc(1, sizeof(directory_t));
	directory_t *working_directory = (directory_t*) calloc(1, sizeof(directory_t));
	inode_t *root_inode = (inode_t*)calloc(1, sizeof(inode_t));
	inode_t *parent_inode = (inode_t*)calloc(1, sizeof(inode_t));
	char path_element[65];

	//start at the root
	if(!memcpy(root_inode, &(fs->inodes[0]), 64)){	//get root inode into working memory
		free(root_directory);
		free(working_directory);
		free(parent_inode);
		free(root_inode);
		return NULL;
	}


	//get the block pointer to root directory from root inode
	unsigned root_directory_block_ptr = root_inode->direct_block_ptr_array[0];

	block_store_read(fs->fs, root_directory_block_ptr, root_directory);		//get the root directory into working memory

	//if root was the only element in path
	if(num_elements == 0){
		free(root_directory);
		free(working_directory);
		free(parent_inode);
		return root_inode;
	}

	memcpy(working_directory, root_directory, 512);		//make a copy of root_directory
	//scan directory records for path element
	for(i = 0; i < num_elements; i++){		//for every element in the path
		int num_entries = working_directory->num_entries;
		dyn_array_extract_front(tokens, path_element);	//get the next path element
		j = 0;
	
		for(j = 0; j < num_entries; j++){	//for every directory entry in the working directory
		

			if(strcmp(path_element, working_directory->records[j].name) == 0){		//if we find a match

				if(working_directory->records[j].type == 0 && !dyn_array_empty(tokens)){	//if a path element along the path is a file, we can't open it
					directory_traversal_frees(root_directory, working_directory, root_inode);
					if(parent_inode != NULL){
						free(parent_inode);
					}
					return NULL;
				}else if(working_directory->records[j].type == 0 && dyn_array_empty(tokens)){	//if a file is found at the end of the path
					memcpy(parent_inode, &(fs->inodes[working_directory->records[j].inode_index]), 64);		//get inode for next directory in path
					block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], working_directory);	//retrieve next directory from storage
					directory_traversal_frees(root_directory, working_directory, root_inode);
					return parent_inode;
				}else if(working_directory->records[j].type == 1 && dyn_array_empty(tokens)){	//if a directory is found at the end of the path
					memcpy(parent_inode, &(fs->inodes[working_directory->records[j].inode_index]), 64);	//get inode for next directory in path
					block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], working_directory);	//retrieve next directory from storage
					directory_traversal_frees(root_directory, working_directory, root_inode);
					return parent_inode;
				}else if(working_directory->records[j].type == 1 && !dyn_array_empty(tokens)){	//if we find a directory along the path, open it and continue
					memcpy(parent_inode, &(fs->inodes[working_directory->records[j].inode_index]), 64);	//get inode for next directory in path
					block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], working_directory);	//retrieve next directory from storage
				}else{
					// printf("UNKNOWN ERROR: FILE NOT FOUND\n");
				}

			}
		}

	}

	directory_traversal_frees(root_directory, working_directory, root_inode);
	if(parent_inode != NULL){
		free(parent_inode);
	}

	return NULL;
}

void directory_traversal_frees(directory_t *root_directory, directory_t *working_directory, inode_t *root_inode){

	if(root_directory != NULL){
		free(root_directory);
	}

	if(working_directory != NULL){
		free(working_directory);
	}
	
	if(root_inode != NULL){
		free(root_inode);
	}

}

///
/// Creates a new file at the specified location
///   Directories along the path that do not exist are NOT created
/// \param fs The F16FS containing the file
/// \param path Absolute path to file to create
/// \param type Type of file to create (regular/directory)
/// \return 0 on success, < 0 on failure
///
int fs_create(F16FS_t *fs, const char *path, file_t type){

	if(fs == NULL || path == NULL || strcmp(path, "") == 0 || strcmp(path, "/") == 0 || path[0] != '/' || (type != 0 && type != 1)){
		return -1;
	}

	int path_length = (int)strlen(path);

	//if the path has a trailing / and no filename
	if(path[path_length-1] == '/' || path_length > 100){
		return -1;
	}

	//parse path to get filename
	dyn_array_t* tokens = parse_path(path);
	//parsing error
	if(tokens == NULL){
		return -1;
	}

	char filename[65];
	//get filename of element at end of path
	dyn_array_extract_back(tokens, filename);

	inode_t* parent_inode = directory_traversal(fs, tokens);

	//if directory_traversal returns NULL
	if(parent_inode == NULL){
		dyn_array_destroy(tokens);
		return -1;
	}	

	if(parent_inode->file_type == 0){	//FS_REGULAR can't be a parent
		dyn_array_destroy(tokens);
		free(parent_inode);
		return -1;
	}

	int num_inodes_in_use = 0;
	int use_flag = 1;
	int free_inode_index;

	//find a free inode for our new file
	while(use_flag == 1){
		if(num_inodes_in_use > 255){		//max number of inodes in use
			free(parent_inode);
			dyn_array_destroy(tokens);
			return -1;
		}
		use_flag = fs->inodes[num_inodes_in_use].use_flag;
		if(use_flag == 0){				//inode not in use
			free_inode_index = num_inodes_in_use;
		}
		num_inodes_in_use++;
	}

	unsigned parent_directory_block_pointer = parent_inode->direct_block_ptr_array[0];

	directory_t *working_directory = (directory_t*) calloc(1, sizeof(directory_t));
	directory_t *parent_directory = (directory_t*) calloc(1, sizeof(directory_t));
	file_record_t *record = (file_record_t*) calloc(1, sizeof(file_record_t));

	//create a new record for the new file to be inserted into parent directory
	strcpy(record->name, filename);
	record->type = type;
	record->inode_index = free_inode_index;

	block_store_read(fs->fs, parent_directory_block_pointer, parent_directory);		//get the parent directory block from storage
	int new_file_block_pointer;
	//allocate a block as starting point for new file

	if((new_file_block_pointer = block_store_allocate(fs->fs)) == 0 && type == 1){
		block_store_release(fs->fs,new_file_block_pointer);
		dyn_array_destroy(tokens);
		free(working_directory);
		free(parent_directory);
		free(record);
		free(parent_inode);		
		return -1;
	}
	if(new_file_block_pointer == 0 && type == 0){
		block_store_release(fs->fs,new_file_block_pointer);
		dyn_array_destroy(tokens);
		free(working_directory);
		free(parent_directory);
		free(record);
		free(parent_inode);	
		return 0;
	}

	int i = 0;
	for(i = 0; i < parent_directory->num_entries; i++){
		if(strcmp(parent_directory->records[i].name, filename) == 0){		//if file already exists
			block_store_release(fs->fs,new_file_block_pointer);
			dyn_array_destroy(tokens);
			free(working_directory);
			free(parent_directory);
			free(record);
			free(parent_inode);
			return -1;
		}
	}


	int num_entries = parent_directory->num_entries;

	if(num_entries == 7){				//if the directory is full
		block_store_release(fs->fs,new_file_block_pointer);
		dyn_array_destroy(tokens);
		free(working_directory);
		free(parent_directory);
		free(record);
		free(parent_inode);
		return -1;
	}

	memcpy(&(parent_directory->records[num_entries]), record, 72);

	parent_directory->num_entries++;	//track number of entries in parent directory

	//write updated parent directory back to storage
	block_store_write(fs->fs, parent_directory_block_pointer, parent_directory);


	//set up new inode for new file
	inode_t* new_file_inode = (inode_t*)calloc(1, sizeof(inode_t));

	new_file_inode->file_type = type;
	new_file_inode->use_flag = 1;
	if(type == 0){
		new_file_inode->file_size = 0;
	}else{
		new_file_inode->file_size = sizeof(directory_t);
	}
	new_file_inode->direct_block_ptr_array[0] = new_file_block_pointer;

	//write new file's inode to inode table
	memcpy(&(fs->inodes[free_inode_index]), new_file_inode, 64);
	//Finally, if the file type is a directory, write a directory structure to the file's allocated block
	if(type == 1){
		block_store_write(fs->fs, new_file_block_pointer, working_directory);
	}

	dyn_array_destroy(tokens);
	free(working_directory);
	free(parent_directory);
	free(record);
	free(parent_inode);
	free(new_file_inode);
	return 0;
}

//tokenize file path into ordered list and return dyn array of tokens
dyn_array_t* parse_path(const char* path){

	char temp[65];
	strcpy(temp, path);
	dyn_array_t *tokens = (dyn_array_t*)dyn_array_create(0, 65, NULL);
	char* token;
	token = strtok(temp, "/");

	while(token != NULL){
		if(strlen(token) > 63){		//if the filename is too big
			dyn_array_destroy(tokens);
			return NULL;
		}
		dyn_array_push_back(tokens, token);
		token = strtok(NULL, "/");
	}

	free(token);
	return tokens;
}
///
/// Opens the specified file for use
///   R/W position is set to the beginning of the file (BOF)
///   Directories cannot be opened
/// \param fs The F16FS containing the file
/// \param path path to the requested file
/// \return file descriptor to the requested file, < 0 on error
///
int fs_open(F16FS_t *fs, const char *path){
	//parameter validation
	if(fs == NULL || path == NULL || strcmp(path, "") == 0 || strcmp(path, "/") == 0 || path[0] != '/'){
		return -1;
	}

	int path_length = (int)strlen(path);
	int i;

	//if the path has a trailing / and no filename
	if(path[path_length-1] == '/'){
		return -1;
	}

	//parse path to get filename
	dyn_array_t* tokens = parse_path(path);
	//parsing error
	if(tokens == NULL){
		return -1;
	}

	char filename[65];
	//get the filename of the ultimate destination
	dyn_array_extract_back(tokens, filename);

	//get parent_inode of ultimate destination
	inode_t* parent_inode = directory_traversal(fs, tokens);	

	if(parent_inode == NULL){
		dyn_array_destroy(tokens);
		return -1;
	}

	directory_t *parent_directory = (directory_t*) calloc(1, sizeof(directory_t));
	file_record_t *record = (file_record_t*) calloc(1, sizeof(file_record_t));
	inode_t *inode_for_open = (inode_t*)calloc(1, sizeof(inode_t));
	int inode_index_for_open;
	
	block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], parent_directory);	//retrieve directory from storage

	//find the inode index of the file to be opened
	for(i = 0; i < parent_directory->num_entries; i++){
		if(strcmp(filename, parent_directory->records[i].name) == 0){
			inode_index_for_open = parent_directory->records[i].inode_index;
		}
	}

	int sentinel = -1;
	i = 0;

	//find a free fd index in our file_descriptor table and set the inode_index
	while(sentinel < 0 && i < 256){
		if(fs->file_descriptors[i].inode_index < 0){
			fs->file_descriptors[i].inode_index = inode_index_for_open;
			sentinel = 1;
		}
		i++;
	}

	//ran out of fd descriptors
	if(i == 256){
		dyn_array_destroy(tokens);
		free(parent_inode);
		free(parent_directory);
		free(record);
		free(inode_for_open);		
		return -1;
	}

	int fd_index;
	if(sentinel == 1){ 		//if an available fd was found
		fd_index = i - 1;
	}

	//make a working copy of the inode for the file to be opened and check if it's a directory which shouldn't be opened
	memcpy(inode_for_open, &(fs->inodes[inode_index_for_open]), 64);
	if(inode_for_open->file_type == 1){
		dyn_array_destroy(tokens);
		free(parent_inode);
		free(parent_directory);
		free(record);
		free(inode_for_open);		
		return -1;
	}


	dyn_array_destroy(tokens);
	free(parent_inode);
	free(parent_directory);
	free(record);
	free(inode_for_open);

	return fd_index;
}

///
/// Closes the given file descriptor
/// \param fs The F16FS containing the file
/// \param fd The file to close
/// \return 0 on success, < 0 on failure
///
int fs_close(F16FS_t *fs, int fd){

	//parameter validation
	if(fs == NULL || fd > 255 || fd < 0){
		return -1;
	}

	//close a valid file descriptor
	if(fs->file_descriptors[fd].inode_index >= 0){
		fs->file_descriptors[fd].inode_index = -1;
		return 0;
	}

	return -1;
}

///
/// Moves the R/W position of the given descriptor to the given location
///   Files cannot be seeked past EOF or before BOF (beginning of file)
///   Seeking past EOF will seek to EOF, seeking before BOF will seek to BOF
/// \param fs The F16FS containing the file
/// \param fd The descriptor to seek
/// \param offset Desired offset relative to whence
/// \param whence Position from which offset is applied
/// \return offset from BOF, < 0 on error
///
off_t fs_seek(F16FS_t *fs, int fd, off_t offset, seek_t whence){
	//parameter validation
	if(fs == NULL || fd < 0 || fd > 256){
		return -1;
	}

	if(whence < 0 || whence > 2){
		// printf("invalid whence!\n");
		return -1;
	}


	int inode_index_for_read = fs->file_descriptors[fd].inode_index;
	unsigned long file_size = fs->inodes[inode_index_for_read].file_size;
	int new_offset = 0;
	//bad fd
	if(fs->file_descriptors[fd].inode_index < 0){
		return -1;
	}

	
	if(whence == FS_SEEK_SET){	//simple: set the seeker to the absolute offset given by the user
		new_offset = offset;
	}else if(whence == FS_SEEK_END){	//set the seeker to EOF + offset
		new_offset = file_size + offset;
	}else{		//otherwise, the user chose FS_SEEK_CUR so take the current offset and add the user defined offset
		new_offset = fs->file_descriptors[fd].offset + offset;
	}
	//if the offset ends up larger than file size, set the seeker to EOF
	if(new_offset > (int)file_size){
		new_offset = file_size;
	}
	//if the offset ends up seeking before beginning of file, set the seeker to the beginning of file
	if(new_offset < 0){
		new_offset = 0;
	}
	//update the offset in fd
	fs->file_descriptors[fd].offset = new_offset;
	return new_offset;

}

///
/// Reads data from the file linked to the given descriptor
///   Reading past EOF returns data up to EOF
///   R/W position in incremented by the number of bytes read
/// \param fs The F16FS containing the file
/// \param fd The file to read from
/// \param dst The buffer to write to
/// \param nbyte The number of bytes to read
/// \return number of bytes read (< nbyte IFF read passes EOF), < 0 on error
///
ssize_t fs_read(F16FS_t *fs, int fd, void *dst, size_t nbyte){
	
	//parameter validation
	if(fs == NULL || fd < 0 || fd > 256 || dst == NULL || (int)nbyte < 0){
		return -1;
	}

	if(nbyte == 0){
		return 0;
	}

	//invalid file descriptor
	if(fs->file_descriptors[fd].inode_index < 0){
		return -1;
	}

	//get the inode index for the file we want to write to via the file descriptor
	inode_t* inode_for_read = (inode_t*)calloc(1, sizeof(inode_t));
	int inode_index_for_read = fs->file_descriptors[fd].inode_index;
	memcpy(inode_for_read, &(fs->inodes[inode_index_for_read]), 64);
	unsigned long file_size = fs->inodes[inode_index_for_read].file_size;
	int block_offset; 
	//find out if we are going to have an offset into a block
	if(file_size == 0 && nbyte % 512 == nbyte){
		block_offset = nbyte;
	}else{
		block_offset = fs->file_descriptors[fd].offset % 512;	//I changed this from fd.offset to file_size
	}	
	char * dst_ptr = (char*)dst;

	int bytes_read = 0;
	int bytes_left_to_read = nbyte;

	//get the location of the block we need to start at
	int read_block_ptr = -1;

	uint8_t* temp_block = (uint8_t*)calloc(1, 512);

	while(bytes_left_to_read > 0){
		//use offset instead of filesize to find the starting block for read (vs the way we do it in write)
		read_block_ptr = get_block_ptr(fs, inode_index_for_read, fs->file_descriptors[fd].offset / 512, 1);

		if(read_block_ptr <= 0){	//get_block_ptr failed (i.e. we ran out of blocks)
			// printf("get_block_ptr failed!\n");
			free(temp_block);
			free(inode_for_read);
			return bytes_read;
		}
		if(file_size < 512){	//initialization read
			block_store_read(fs->fs, read_block_ptr, dst_ptr);
			dst_ptr += nbyte;
			bytes_read += nbyte;
			fs->file_descriptors[fd].offset += nbyte;
			bytes_left_to_read -= nbyte;
			block_offset = fs->file_descriptors[fd].offset % 512;
		}else if(block_offset != 0){		//read from offset within a block
			block_store_read(fs->fs, read_block_ptr, temp_block);
			memcpy(dst_ptr, temp_block + block_offset, 512 - block_offset);
			dst_ptr += 512 - block_offset;
			bytes_read += 512 - block_offset;
			fs->file_descriptors[fd].offset += 512 - block_offset;
			bytes_left_to_read -= 512 - block_offset;
			block_offset = 0;
		}else{		//read a normal 512 block
			block_store_read(fs->fs, read_block_ptr, dst_ptr);
			dst_ptr += 512;
			bytes_read += 512;
			fs->file_descriptors[fd].offset += 512;
			bytes_left_to_read -= 512;	
		}

	}

	//fix clerical errors from corner cases
	if(bytes_left_to_read < 0){
		bytes_read += bytes_left_to_read;
		fs->file_descriptors[fd].offset += bytes_left_to_read;
	}

	free(inode_for_read);
	free(temp_block);
	return bytes_read;	


}

///
/// Writes data from given buffer to the file linked to the descriptor
///   Writing past EOF extends the file
///   Writing inside a file overwrites existing data
///   R/W position in incremented by the number of bytes written
///   If there is not enough free space for a full write, as much data as possible will be written
/// \param fs The F16FS containing the file
/// \param fd The file to write to
/// \param dst The buffer to read from
/// \param nbyte The number of bytes to write
/// \return number of bytes written (< nbyte IFF out of space), < 0 on error
///
ssize_t fs_write(F16FS_t *fs, int fd, const void *src, size_t nbyte){
	//parameter validation
	if(fs == NULL || fd < 0 || fd > 256 || src == NULL || (int)nbyte < 0){
		return -1;
	}

	if(nbyte == 0){
		return 0;
	}

	//invalid file descriptor
	if(fs->file_descriptors[fd].inode_index < 0){
		return -1;
	}

	//get the inode index for the file we want to write to via the file descriptor
	inode_t* inode_for_write = (inode_t*)calloc(1, sizeof(inode_t));
	int inode_index_for_write = fs->file_descriptors[fd].inode_index;
	memcpy(inode_for_write, &(fs->inodes[inode_index_for_write]), 64);
	unsigned long file_size = fs->inodes[inode_index_for_write].file_size;
	int block_offset; 
	//find out if we need to offset to within a block
	if(file_size == 0 && nbyte % 512 == nbyte){
		block_offset = nbyte;
	}else{
		block_offset = file_size % 512;	
	}	
	//cast src so we can use ptr arithmetic
	char * src_ptr = (char*)src;

	int bytes_written = 0;
	int bytes_left_to_write = nbyte;

	//get the location of the block we need to start at
	int write_block_ptr = -1;

	uint8_t* temp_block = (uint8_t*)calloc(1, 512);

	while(bytes_left_to_write > 0){
		//by using file size / 512 we can get the block at the current end of file
		write_block_ptr = get_block_ptr(fs, inode_index_for_write, fs->inodes[inode_index_for_write].file_size / 512, 0);
		if(write_block_ptr < 0){	//get_block_ptr failed (i.e. we probably ran out of blocks)
			// printf("get_block_ptr failed!\n");
			free(temp_block);
			free(inode_for_write);
			return bytes_written;
		}
		if(file_size < 512 && nbyte < 512){		//initialization write
			block_store_write(fs->fs, write_block_ptr, src_ptr);
			src_ptr += nbyte;
			bytes_written += nbyte;
			fs->inodes[inode_index_for_write].file_size += nbyte;
			bytes_left_to_write -= nbyte;
			block_offset = bytes_left_to_write % 512;
		}else if(block_offset != 0){	//write to inside of a block at an offset via read-modify-write
			block_store_read(fs->fs, write_block_ptr, temp_block);
			memcpy(temp_block + block_offset, src_ptr, 512 - (512 - block_offset));
			block_store_write(fs->fs, write_block_ptr, temp_block);
			src_ptr += 512 - (512 - block_offset);
			bytes_written += 512 - (512 - block_offset);
			fs->inodes[inode_index_for_write].file_size += 512 - (512 - block_offset);
			bytes_left_to_write -= 512 - (512 - block_offset);
			block_offset = bytes_left_to_write % 512;
		}else{		//normal write of 512 byte block
			block_store_write(fs->fs, write_block_ptr, src_ptr);
			src_ptr += 512;
			bytes_written += 512;
			fs->inodes[inode_index_for_write].file_size += 512;
			bytes_left_to_write -= 512;	
		}
	}

	if(bytes_left_to_write < 0){
		bytes_written += bytes_left_to_write;
		fs->inodes[inode_index_for_write].file_size += bytes_left_to_write;
	}
	free(temp_block);
	free(inode_for_write);
	return bytes_written;

}

int get_block_ptr(F16FS_t* fs, int inode_index, int block_to_start_at, uint8_t read_write_flag){

	unsigned short block_ptr_array[256] = {0};
	unsigned short double_indirect_block_ptr_array[256] = {0};
	unsigned short double_IBP_index = 0;
	unsigned short subarray_index = 0;
	unsigned short block_ptr;

	//if we need a double indirect
	if(block_to_start_at >= 262){
		//if our double_indirect_block_ptr is uninitialized, initialize it by allocating a block full of indirect block pointers
		if(fs->inodes[inode_index].double_indirect_block_ptr == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR: ran out of blocks!\n");
				return -1;			
			}
			block_store_write(fs->fs, block_ptr, double_indirect_block_ptr_array);
			fs->inodes[inode_index].double_indirect_block_ptr = block_ptr;
		}
		//copy out double indirect block ptr block to working memory array
		block_store_read(fs->fs, fs->inodes[inode_index].double_indirect_block_ptr, double_indirect_block_ptr_array);
		//now we need to find the index to reference in our double IBP array to get to the appropriate sub-array of block pointers
		double_IBP_index = (block_to_start_at - 262) / 256;
		//if the subarray is unitiliazed, allocate and write a block ptr sub-array
		if(double_indirect_block_ptr_array[double_IBP_index] == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR 1000: ran out of blocks!\n");
				return -1;			
			}
			block_store_write(fs->fs, block_ptr, block_ptr_array);
			double_indirect_block_ptr_array[double_IBP_index] = block_ptr;
			block_store_write(fs->fs, fs->inodes[inode_index].double_indirect_block_ptr, double_indirect_block_ptr_array);
		}
		//get index for ultimate double_block_pointer sub-array we need to index into
		subarray_index = (block_to_start_at - 262 - 256*double_IBP_index);
		//read out the appropriate block ptr sub-array
		block_store_read(fs->fs, double_indirect_block_ptr_array[double_IBP_index], block_ptr_array);
		//if the block we're after is unitialized, allocate it
		if(block_ptr_array[subarray_index] == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR 1000: ran out of blocks!\n");	
				return -1;			
			}	
			block_ptr_array[subarray_index] = block_ptr;
			block_store_write(fs->fs, double_indirect_block_ptr_array[double_IBP_index], block_ptr_array);	
		}
		block_ptr = block_ptr_array[subarray_index];

	}
	if(block_to_start_at >= 6 && block_to_start_at < 262){		//if we need a single indirect
		if(fs->inodes[inode_index].indirect_block_ptr == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR: ran out of blocks!\n");
				return -1;				
			}
			block_store_write(fs->fs, block_ptr, block_ptr_array);
			fs->inodes[inode_index].indirect_block_ptr = block_ptr;
		}
		block_store_read(fs->fs, fs->inodes[inode_index].indirect_block_ptr, block_ptr_array);
		//if we need to initialize a block for our indirect_block_ptr_array index
		if(block_ptr_array[block_to_start_at - 6] == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR: ran out of blocks!\n");
				return -1;				
			}
			block_ptr_array[block_to_start_at - 6] = block_ptr;
			block_store_write(fs->fs, fs->inodes[inode_index].indirect_block_ptr, block_ptr_array);
		}
		block_ptr = block_ptr_array[block_to_start_at - 6];
	}
	if(block_to_start_at < 6){		//if we need a direct block pointer
		//get a direct block pointer
		if(fs->inodes[inode_index].direct_block_ptr_array[block_to_start_at] == 0 && read_write_flag == 0){
			if((block_ptr = block_store_allocate(fs->fs)) == 0){
				// printf("ERROR: ran out of blocks!\n");
				return -1;				
			}
			fs->inodes[inode_index].direct_block_ptr_array[block_to_start_at] = block_ptr;
		}		
		block_ptr = fs->inodes[inode_index].direct_block_ptr_array[block_to_start_at];
	}


	return block_ptr;

}

///
/// Deletes the specified file
///   Directories can only be removed when empty
///   Using a descriptor to a file that was deleted is undefined
/// \param fs The F16FS containing the file
/// \param path Absolute path to file to remove
/// \return 0 on success, < 0 on error
///
int fs_remove(F16FS_t *fs, const char *path){
	//parameter validation
	if(fs == NULL || path == NULL || strcmp(path, "") == 0 || strcmp(path, "/") == 0 || path[0] != '/'){
		return -1;
	}

	int path_length = (int)strlen(path);
	int i;

	//if the path has a trailing / and no filename
	if(path[path_length-1] == '/'){
		return -1;
	}

	//parse path to get filename
	dyn_array_t* tokens = parse_path(path);
	//parsing error
	if(tokens == NULL){
		return -1;
	}

	char filename[65];
	//get the filename of the ultimate destination
	dyn_array_extract_back(tokens, filename);
	

	//get parent_inode of ultimate destination
	inode_t* parent_inode = directory_traversal(fs, tokens);	
	

	if(parent_inode == NULL){
		dyn_array_destroy(tokens);
		return -1;
	}

	directory_t *parent_directory = (directory_t*) calloc(1, sizeof(directory_t));
	directory_t *working_directory = (directory_t*) calloc(1, sizeof(directory_t));
	file_record_t *blanked_record = (file_record_t*) calloc(1, sizeof(file_record_t));
	inode_t *inode_for_removal = (inode_t*)calloc(1, sizeof(inode_t));
	inode_t *blanked_inode = (inode_t*)calloc(1, sizeof(inode_t));
	int inode_index_for_removal = 999;
	int records_index = 999;
	block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], parent_directory);	//retrieve directory from storage

	//if the parent directory is empty then the file doesn't exist to be removed
	if(parent_directory->num_entries == 0){
		free(parent_directory);
		free(parent_inode);
		free(working_directory);
		free(blanked_record);
		free(inode_for_removal);
		free(blanked_inode);
		dyn_array_destroy(tokens);		
		// printf("ERROR: File not found!\n");
		return -1;
	}
	//find the inode index of the file to be opened
	for(i = 0; i < parent_directory->num_entries; i++){
		if(strcmp(filename, parent_directory->records[i].name) == 0){
			records_index = i;
			inode_index_for_removal = parent_directory->records[i].inode_index;
		}
	}
	//error check inode index
	if(inode_index_for_removal < 0 || inode_index_for_removal > 256){
		// printf("inode out of bounds\n");
		free(parent_directory);
		free(parent_inode);
		free(working_directory);
		free(blanked_record);
		free(inode_for_removal);
		free(blanked_inode);
		dyn_array_destroy(tokens);
		return 0;
	}
	i = 0;
	//make a working copy of the inode for the file to be removed
	memcpy(inode_for_removal, &(fs->inodes[inode_index_for_removal]), 64);
	//if it's a file
	if(inode_for_removal->file_type == 0){
		//swap the last record in the parent directory into this spot we want to delete
		if((parent_directory->num_entries-1) != records_index){
			memcpy(&(parent_directory->records[records_index]),&(parent_directory->records[parent_directory->num_entries-1]), 72);
		}
		memcpy(&(parent_directory->records[parent_directory->num_entries]), blanked_record, 72);	//blank the last record just to be safe
		parent_directory->num_entries--;
		block_store_write(fs->fs, parent_inode->direct_block_ptr_array[0], parent_directory);
	
		//free all the blocks used by the file
		unsigned long file_size = fs->inodes[inode_index_for_removal].file_size;
		uint8_t read_block_ptr = 99;
		while(file_size > 0){
			//use offset instead of filesize to find the starting block for read (vs the way we do it in write)
			if(file_size <= 512){
				block_store_release(fs->fs, inode_for_removal->direct_block_ptr_array[0]);;
				file_size -= 512;
			}else{
				read_block_ptr = get_block_ptr(fs, inode_index_for_removal, file_size / 512 + 1, 1);
				// if(read_block_ptr <= 0){	//get_block_ptr failed (i.e. we ran out of blocks)
				// 	printf("get_block_ptr failed!\n");
				// 	return -1;
				// }
				block_store_release(fs->fs, read_block_ptr);
				file_size -= 512;
			}
		}
		//blank the inode (setting it's state to unused at the same time)
		memcpy(&(fs->inodes[inode_index_for_removal]), blanked_inode, 64);
	}else{		//otherwise it's a directory and we have to see if it's empty first
		block_store_read(fs->fs, inode_for_removal->direct_block_ptr_array[0], working_directory);
		if(working_directory->num_entries > 0){		//can't delete a directory with files in it
			// printf("ERROR: Cannot delete directory that is not empty!\n");
			free(parent_directory);
			free(working_directory);
			free(parent_inode);
			free(blanked_record);
			free(inode_for_removal);
			free(blanked_inode);
			dyn_array_destroy(tokens);
			return -1;
		}
		//free the block
		block_store_release(fs->fs, inode_for_removal->direct_block_ptr_array[0]);
		//blank the inode (setting it's state to unused at the same time)
		memcpy(&(fs->inodes[inode_index_for_removal]), blanked_inode, 64);
		//swap the last record in the parent directory into this spot we want to delete
		if((parent_directory->num_entries-1) != records_index){
			memcpy(&(parent_directory->records[records_index]),&(parent_directory->records[parent_directory->num_entries-1]), 72);
		}
		memcpy(&(parent_directory->records[parent_directory->num_entries]), blanked_record, 72);	//blank the last record just to be safe
		parent_directory->num_entries--;
		block_store_write(fs->fs, parent_inode->direct_block_ptr_array[0], parent_directory);
		memcpy(&(fs->inodes[inode_index_for_removal]), blanked_inode, 64);
	}



	free(parent_directory);
	free(parent_inode);
	free(working_directory);
	free(blanked_record);
	free(inode_for_removal);
	free(blanked_inode);
	dyn_array_destroy(tokens);
	return 0;

}

///
/// Populates a dyn_array with information about the files in a directory
///   Array contains up to 7 file_record_t structures
/// \param fs The F16FS containing the file
/// \param path Absolute path to the directory to inspect
/// \return dyn_array of file records, NULL on error
///

dyn_array_t *fs_get_dir(F16FS_t *fs, const char *path){
	if(fs == NULL || path == NULL || strcmp(path, "") == 0 || path[0] != '/'){
		return NULL;
	}

	int path_length = (int)strlen(path);

	//if the path has a trailing / and no filename
	if((path[path_length-1] == '/' && strcmp(path, "/") != 0 )|| path_length > 100){
		return NULL;
	}

	//parse path to get filename
	dyn_array_t* tokens = parse_path(path);
	//parsing error
	if(tokens == NULL){
		return NULL;
	}

	//get filename of ultimate destination of path
	char filename[65];
	dyn_array_extract_back(tokens, filename);

	//get the parent inode
	inode_t* parent_inode = directory_traversal(fs, tokens);

	//if directory traversal failed
	if(parent_inode == NULL){
		dyn_array_destroy(tokens);
		return NULL;
	}	

	// printf("Contents of parent_inode returned to fs_open: type = %d; file size = %lu; block ptr = %u\n", parent_inode->file_type, parent_inode->file_size, parent_inode->direct_block_ptr_array[0]);


	directory_t *parent_directory = (directory_t*)calloc(1, sizeof(directory_t));
	directory_t *working_directory = (directory_t*)calloc(1, sizeof(directory_t));
	file_record_t *record = (file_record_t*)calloc(1, sizeof(file_record_t));
	inode_t *inode_for_open = (inode_t*)calloc(1, sizeof(inode_t));
	dyn_array_t *dir_info = dyn_array_create(0, sizeof(file_record_t), NULL);
	int inode_index_for_open, i;
	
	block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], parent_directory);	//retrieve parent directory from storage

	//special logic for if the path to open is root since my directory_traversal() returns the parent directory of the final path destination
	if(strcmp(path, "/") == 0){

		block_store_read(fs->fs, parent_inode->direct_block_ptr_array[0], working_directory);	//retrieve root directory from storage

		if(working_directory->num_entries == 0){		//if the folder is empty
			dyn_array_destroy(tokens);
			free(parent_inode);
			free(parent_directory);
			free(working_directory);
			free(record);
			free(inode_for_open);
			return dir_info;
		}		
		//if the folder isn't empty, push the file records it contains to a dyn array
		for(i = 0; i < working_directory->num_entries; i++){
			dyn_array_push_back(dir_info, &(working_directory->records[i]));
		}

		dyn_array_destroy(tokens);
		free(parent_inode);
		free(parent_directory);
		free(working_directory);
		free(record);
		free(inode_for_open);		
		return dir_info;
	}


	//get the inode index of the folder to be opened
	for(i = 0; i < parent_directory->num_entries; i++){
		if(strcmp(filename, parent_directory->records[i].name) == 0){
			inode_index_for_open = parent_directory->records[i].inode_index;
		}
	}

	//retrieve inode of directory we want
	memcpy(inode_for_open, &(fs->inodes[inode_index_for_open]), 64);		

	if(inode_for_open->file_type == 0){		//we can't return dir info from a FS_REGULAR file type
		dyn_array_destroy(tokens);
		dyn_array_destroy(dir_info);
		free(parent_inode);
		free(parent_directory);
		free(working_directory);
		free(record);
		free(inode_for_open);		
		return NULL;
	}	

	block_store_read(fs->fs, inode_for_open->direct_block_ptr_array[0], working_directory);	//retrieve directory from storage

	if(working_directory->num_entries == 0){		//if the directory is empty
		dyn_array_destroy(tokens);
		free(parent_inode);
		free(parent_directory);
		free(working_directory);
		free(record);
		free(inode_for_open);
		return dir_info;
	}

	//push the directory entries to a dyn array
	for(i = 0; i < working_directory->num_entries; i++){
		dyn_array_push_back(dir_info, &(working_directory->records[i]));
	}

	
	dyn_array_destroy(tokens);
	free(parent_inode);
	free(parent_directory);
	free(working_directory);
	free(record);
	free(inode_for_open);

	return dir_info;
}

///
/// !!! Graduate Level/Undergrad Bonus !!!
/// !!! Activate tests from the cmake !!!
///
/// Moves the file from one location to the other
///   Moving files does not affect open descriptors
/// \param fs The F16FS containing the file
/// \param src Absolute path of the file to move
/// \param dst Absolute path to move the file to
/// \return 0 on success, < 0 on error
///

int fs_move(F16FS_t *fs, const char *src, const char *dst){
	//parameter validation
	if(fs == NULL || src == NULL || dst == NULL || strcmp(src, "") == 0 || strcmp(src, "/") == 0 || src[0] != '/' || strcmp(dst, "") == 0 || strcmp(dst, "/") == 0 || dst[0] != '/'){
		return -1;
	}

	int src_path_length = (int)strlen(src);
	int dst_path_length = (int)strlen(dst);
	int i;

	//if the path has a trailing / and no filename
	if(src[src_path_length-1] == '/' || dst[dst_path_length-1] == '/'){
		return -1;
	}

	//parse path to get filenames
	dyn_array_t* src_tokens = parse_path(src);
	dyn_array_t* dst_tokens = parse_path(dst);

	//parsing error
	if(src_tokens == NULL || dst_tokens == NULL){
		return -1;
	}

	char dst_filename[65];
	char dst_folder_filename[65] = {0};
	char src_filename[65];
	//get the filenames
	int dst_tokens_length = dyn_array_size(dst_tokens);
	strcpy(dst_filename, dyn_array_at(dst_tokens, dst_tokens_length - 1));
	if(dst_tokens_length >= 2){
		strcpy(dst_folder_filename, dyn_array_at(dst_tokens, dst_tokens_length - 2));
	}
	dyn_array_extract_back(src_tokens, src_filename);

	// printf("src_filename: %s\n", src_filename);
	// printf("dst_filename: %s\n", dst_filename);

	//check if we're trying to move a directory into itself
	if(strcmp(src_filename, dst_folder_filename) == 0){
		// printf("ERROR: trying to move a directory into itself!\n");
		dyn_array_destroy(src_tokens);
		dyn_array_destroy(dst_tokens);
		return -1;
	}

	dyn_array_extract_back(dst_tokens, dst_filename);

	//get parent_inodes
	inode_t* src_parent_inode = directory_traversal(fs, src_tokens);	//parent inode of src, which will give us parent directory...perfect
	inode_t* dst_parent_inode = directory_traversal(fs, dst_tokens);	//this is the parent inode of where we will move the file to, so we need to move down one more level

	//check for valid parent inodes
	if(src_parent_inode == NULL || dst_parent_inode == NULL){
		free(src_parent_inode);
		dyn_array_destroy(src_tokens);
		dyn_array_destroy(dst_tokens);
		return -1;
	}

	directory_t* src_parent_directory = (directory_t*) calloc(1, sizeof(directory_t));
	directory_t* dst_parent_directory = (directory_t*) calloc(1, sizeof(directory_t));
	block_store_read(fs->fs, src_parent_inode->direct_block_ptr_array[0], src_parent_directory);	//get src parent directory
	block_store_read(fs->fs, dst_parent_inode->direct_block_ptr_array[0], dst_parent_directory);	//get dst parent directory

	//check if dst already exists
	for(i = 0; i < dst_parent_directory->num_entries; i++){
		if(strcmp(dst_parent_directory->records[i].name, dst_filename) == 0){
			// printf("Error: dst exists!\n");
			dyn_array_destroy(src_tokens);
			dyn_array_destroy(dst_tokens);
			free(src_parent_inode);
			free(dst_parent_inode);
			free(src_parent_directory);
			free(dst_parent_directory);
			return -1;
		}
	}

	//verify src exists
	bool src_exists = false;
	for(i = 0; i < src_parent_directory->num_entries; i++){
		if(strcmp(src_parent_directory->records[i].name, src_filename) == 0){
			src_exists = true;
		}
	}	
	if(!src_exists){
		dyn_array_destroy(src_tokens);
		dyn_array_destroy(dst_tokens);
		free(src_parent_inode);
		free(dst_parent_inode);
		free(src_parent_directory);
		free(dst_parent_directory);		
		return -1;
	}

	//check if src filename already exists at dst
	for(i = 0; i < dst_parent_directory->num_entries; i++){
		if(strcmp(dst_parent_directory->records[i].name, src_filename) == 0){
			// printf("ERROR: src filename already exists at dst!\n");
			dyn_array_destroy(src_tokens);
			dyn_array_destroy(dst_tokens);
			free(src_parent_inode);
			free(dst_parent_inode);
			free(src_parent_directory);
			free(dst_parent_directory);			
			return -1;
		}
	}

	int src_directory_record_index = 0;

	//find the location of our file to move in it's parent directory's list of entries
	for(i = 0; i < src_parent_directory->num_entries; i++){
		if(strcmp(src_parent_directory->records[i].name, src_filename) == 0){
			src_directory_record_index = i;
		}
	}

	//copy the directory entry for file to move from src to destination
	memcpy(&(dst_parent_directory->records[dst_parent_directory->num_entries]), &(src_parent_directory->records[src_directory_record_index]), 72);
	strcpy(dst_parent_directory->records[dst_parent_directory->num_entries].name, dst_filename);
	dst_parent_directory->num_entries++;

	//if the src directory's record index is not the last in the parent directory, then we need to adjust the record entries in that directory to fill the empty spot
	if(src_parent_directory->num_entries != src_directory_record_index + 1){
		memcpy(&(src_parent_directory->records[src_directory_record_index]), &(src_parent_directory->records[(src_parent_directory->num_entries) - 1]), 72);
	}
	src_parent_directory->num_entries--;

	//write modified directories back to storage
	block_store_write(fs->fs, src_parent_inode->direct_block_ptr_array[0], src_parent_directory);
	block_store_write(fs->fs, dst_parent_inode->direct_block_ptr_array[0], dst_parent_directory);

	dyn_array_destroy(src_tokens);
	dyn_array_destroy(dst_tokens);
	free(src_parent_inode);
	free(dst_parent_inode);
	free(src_parent_directory);
	free(dst_parent_directory);
	return 0;


}

///
/// !!! Graduate Level/Undergrad Bonus !!!
/// !!! Activate tests from the cmake !!!
///
/// Creates a hardlink at dst pointing to the file at src
/// \param fs The F16FS containing the file
/// \param src Absolute path of the file to link to
/// \param dst Absolute path to the link to create
/// \return 0 on success, < 0 on error
///

int fs_link(F16FS_t *fs, const char *src, const char *dst){
	if(fs && src && dst){
		return -1;
	}
	return -1;
}