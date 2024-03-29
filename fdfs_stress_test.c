#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "fdfs_client.h"
#include "logger.h"
#include "thread_pool.h"

#include <sys/poll.h>
#include "fdfs_global.h"
#include "sockopt.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <fcntl.h>

#include "share_net.h"

#define default_file_list "fdfs_stress_test_file_list"
int g_wait_for_TIMEWAIT_sec = 1;

typedef struct UPLOAD_TEST_INFO_T
{
	char file_names[200000][128];
	int name_counts;
	int success_count;
	int fail_count;
	int all_count;
	long long upload_size;
	int have_upload_count;
	int upload_count;
	time_t start_time;
	int all_sec;
}upload_test_info_t;

typedef struct DOWNLOAD_TEST_INFO_T
{
	char file_names[200000][128];
	//double every_sec[200000];
	int name_counts;
	int success_count;
	int fail_count;
	int all_count;
	long long download_size;
	int have_download_count;
	int download_count;
	time_t start_time;
	int all_sec;
}download_test_info_t;

typedef struct DOWNLOAD_TEST_FILE_INFO_T
{
	nb_data_t data;	
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 128];
	char in_buff[sizeof(TrackerHeader) + TRACKER_QUERY_STORAGE_FETCH_BODY_LEN];
	char temp[RECV_BUFF_SIZE];
	ConnectionInfo *conn;
	int64_t in_bytes;
	TrackerHeader resp;
	int result;
	ConnectionInfo *TrackerServer;
	ConnectionInfo pNewStorage;
	TrackerServerGroup tracker_group;
	bool get_connection_to_storage;

	int file_offset;
	int out_bytes;
	TrackerHeader *pHeader;
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 128];
	const char *remote_filename;
	int filename_len;
	aeEventLoop *eventLoop;
	ConnectionInfo *servers;
	char *download_filename;
}download_test_file_info_t;
	
char *g_conf_filename;
char g_rubbish[40*1024*1024];
FILE *g_file_list_fp;
IniContext g_iniContext;

typedef struct DOWNLOAD_TEST_INFO_NB_T
{
	aeEventLoop *eventLoop[128];
	int eventLoopCounts;
	pthread_t thread[128];
}download_test_info_nb_t;

download_test_info_nb_t g_download_test_info_nb;

upload_test_info_t g_upload_test_info;
download_test_info_t g_download_test_info;

static int create_test_files(int file_num,int file_size);
static void get_upload_file_names(char *dir_name);
static void upload_calculate();
static void *upload_thread(void *arg);
static int upload_file_by_filename(char *file_name);
int upload_test(int file_num,int client_num);

static void get_download_file_names(char *file_list);
static void download_calculate();
static void *download_thread(void *arg);
static int tcprecvdata_nb_ex_without_writing_buf(int sock, void *data, const int size, \
		const int timeout, int *count);
static int fdfs_recv_response_without_writing_buf(ConnectionInfo *pTrackerServer, \
		char **buff, const int buff_size, \
		int64_t *in_bytes);
static int storage_do_download_file_ex_without_writing_disk(ConnectionInfo *pTrackerServer, \
		ConnectionInfo *pStorageServer, \
		const int download_type, \
		const char *group_name, const char *remote_filename, \
		const int64_t file_offset, const int64_t download_bytes, \
		char **file_buff, void *arg, int64_t *file_size);
static int download_file_by_filename(char *file_name);
int download_test(char *file_list,int client_num);

static int storage_get_connection_test_nb(ConnectionInfo *pTrackerServer, \
		ConnectionInfo **ppStorageServer, const byte cmd, \
		const char *group_name, const char *filename, \
		ConnectionInfo *pNewStorage, bool *new_connection, \
		download_test_file_info_t *pTracker);
static void storage_get_connection_test_nb2(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);
static void storage_get_connection_test_nb3(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);
static void storage_get_connection_test_nb4(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);
int download_by_filename_nb(download_test_file_info_t *pTracker,char *file_name,aeEventLoop *eventLoop);
static void download_by_filename_nb2(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);
static void download_by_filename_nb3(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);
static void download_by_filename_final(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask);

static void usage(char *argv[]);

static int fdfs_get_params_from_tracker(bool *use_storage_id)
{
        IniContext iniContext;
	int result;
	bool continue_flag;

	continue_flag = false;
	if ((result=fdfs_get_ini_context_from_tracker(&g_tracker_group, \
		&iniContext, &continue_flag, false, NULL)) != 0)
        {
                return result;
        }

	*use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				&iniContext, false);
        iniFreeContext(&iniContext);

	if (*use_storage_id)
	{
		result = fdfs_get_storage_ids_from_tracker_group( \
				&g_tracker_group);
	}

        return result;
}

static int fdfs_client_do_init_ex(TrackerServerGroup *pTrackerGroup, \
		const char *conf_filename, IniContext *iniContext)
{
	char *pBasePath;
	int result;
	bool use_storage_id = false;
	bool load_fdfs_parameters_from_tracker;

	pBasePath = iniGetStrValue(NULL, "base_path", iniContext);
	if (pBasePath == NULL)
	{
		strcpy(g_fdfs_base_path, "/tmp");
	}
	else
	{
		snprintf(g_fdfs_base_path, sizeof(g_fdfs_base_path), 
			"%s", pBasePath);
		chopPath(g_fdfs_base_path);
		if (!fileExists(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" can't be accessed, error info: %s", \
				__LINE__, g_fdfs_base_path, STRERROR(errno));
			return errno != 0 ? errno : ENOENT;
		}
		if (!isDir(g_fdfs_base_path))
		{
			logError("file: "__FILE__", line: %d, " \
				"\"%s\" is not a directory!", \
				__LINE__, g_fdfs_base_path);
			return ENOTDIR;
		}
	}

	g_fdfs_connect_timeout = iniGetIntValue(NULL, "connect_timeout", \
				iniContext, DEFAULT_CONNECT_TIMEOUT);
	if (g_fdfs_connect_timeout <= 0)
	{
		g_fdfs_connect_timeout = DEFAULT_CONNECT_TIMEOUT;
	}

	g_fdfs_network_timeout = iniGetIntValue(NULL, "network_timeout", \
				iniContext, DEFAULT_NETWORK_TIMEOUT);
	if (g_fdfs_network_timeout <= 0)
	{
		g_fdfs_network_timeout = DEFAULT_NETWORK_TIMEOUT;
	}

	if ((result=fdfs_load_tracker_group_ex(pTrackerGroup, \
			conf_filename, iniContext)) != 0)
	{
		return result;
	}

	g_anti_steal_token = iniGetBoolValue(NULL, \
				"http.anti_steal.check_token", \
				iniContext, false);
	if (g_anti_steal_token)
	{
		char *anti_steal_secret_key;

		anti_steal_secret_key = iniGetStrValue(NULL, \
					"http.anti_steal.secret_key", \
					iniContext);
		if (anti_steal_secret_key == NULL || \
			*anti_steal_secret_key == '\0')
		{
			logError("file: "__FILE__", line: %d, " \
				"param \"http.anti_steal.secret_key\""\
				" not exist or is empty", __LINE__);
			return EINVAL;
		}

		buffer_strcpy(&g_anti_steal_secret_key, anti_steal_secret_key);
	}

	g_tracker_server_http_port = iniGetIntValue(NULL, \
				"http.tracker_server_port", \
				iniContext, 80);
	if (g_tracker_server_http_port <= 0)
	{
		g_tracker_server_http_port = 80;
	}

	if ((result=fdfs_connection_pool_init(conf_filename, iniContext)) != 0)
	{
		return result;
	}

	load_fdfs_parameters_from_tracker = iniGetBoolValue(NULL, \
				"load_fdfs_parameters_from_tracker", \
				iniContext, false);
	if (load_fdfs_parameters_from_tracker)
	{
		fdfs_get_params_from_tracker(&use_storage_id);
	}
	else
	{
		use_storage_id = iniGetBoolValue(NULL, "use_storage_id", \
				iniContext, false);
		if (use_storage_id)
		{
			result = fdfs_load_storage_ids_from_file( \
					conf_filename, iniContext);
		}
	}

#ifdef DEBUG_FLAG
	logDebug("base_path=%s, " \
		"connect_timeout=%d, "\
		"network_timeout=%d, "\
		"tracker_server_count=%d, " \
		"anti_steal_token=%d, " \
		"anti_steal_secret_key length=%d, " \
		"use_connection_pool=%d, " \
		"g_connection_pool_max_idle_time=%ds, " \
		"use_storage_id=%d, storage server id count: %d\n", \
		g_fdfs_base_path, g_fdfs_connect_timeout, \
		g_fdfs_network_timeout, pTrackerGroup->server_count, \
		g_anti_steal_token, g_anti_steal_secret_key.length, \
		g_use_connection_pool, g_connection_pool_max_idle_time, \
		use_storage_id, g_storage_id_count);
#endif

	return 0;
}

static int create_test_files(int file_num,int file_size)
{
	FILE *fp;
	int file_count;
	char file_name[10];
	DIR *p_dir;

	memset(file_name,0,10);
	//srand((unsigned int)time(NULL));
	
	if(mkdir("temp",0777) < 0)
	{
		fprintf(stderr,"mkdir error!");
		fflush(stderr);
		return -1;
	}
	if((p_dir = opendir("temp")) == NULL)
	{
		fprintf(stderr,"---->can\'t open %s\n","temp");
		fflush(stderr);
		return -1;		
	}
	if(chdir("temp") < 0)
		return -1;
	for(file_count = 0;file_count != file_num;++file_count)
	{
		sprintf(file_name,"%d",file_count);
		fp = fopen(file_name,"w+");
		//fwrite(g_rubbish,1,rand()%file_size+file_size,fp);
		fwrite(g_rubbish,1,file_size,fp);
		fclose(fp);
	}
	if(chdir("..") < 0)
		return -1;
	closedir(p_dir);
	
	return 0;
}

static void get_upload_file_names(char *dir_name)
{
	DIR *p_dir;
	struct dirent *p_dirent;
	struct stat statbuf;

	p_dirent = NULL;
	if((p_dir = opendir(dir_name)) == NULL)
	{
		fprintf(stderr,"---->can\'t open %s\n",dir_name);
		fflush(stderr);
		return ;		
	}
	if(chdir(dir_name) < 0)
		return ;
	while((p_dirent = readdir(p_dir)) != NULL)
	{
		stat(p_dirent->d_name,&statbuf);
		if(S_ISREG(statbuf.st_mode))
		{
			//g_upload_test_info.file_names[g_upload_test_info.name_counts] = malloc(sizeof(dir_name)+1+strlen(p_dirent->d_name)+1);
			memcpy(g_upload_test_info.file_names[g_upload_test_info.name_counts],dir_name,strlen(dir_name)+1);
			strcat(g_upload_test_info.file_names[g_upload_test_info.name_counts],"/");
			strcat(g_upload_test_info.file_names[g_upload_test_info.name_counts],p_dirent->d_name);
			//logDebug("%s",g_upload_test_info.file_names[g_upload_test_info.name_counts]);
			g_upload_test_info.name_counts++;
		}
	}
	if(chdir("..") < 0)
		return ;
	//logDebug("\n");
	closedir(p_dir);
	//logDebug("get_file_names end\n");
	//fflush(stdout);
	
	return ;
}

static void upload_calculate()
{
	g_upload_test_info.all_sec = time(NULL)-g_upload_test_info.start_time;
	printf("upload_size %lld,upload_sec %d,upload_speed %fM/s\n"
		"return with control+C\n",\
		g_upload_test_info.upload_size,g_upload_test_info.all_sec,\
		g_upload_test_info.upload_size/1048576/(float)g_upload_test_info.all_sec);
	fflush(stdout);

	return ;
}

static int upload_file_by_filename(char *local_filename)
{
	char group_name[FDFS_GROUP_NAME_MAX_LEN + 1];
	ConnectionInfo *pTrackerServer;
	int result;
	int store_path_index;
	ConnectionInfo storageServer;
	char file_id[128];

	TrackerServerGroup tracker_group;

	char remote_filename[128];
	const char *file_ext_name;
	struct stat stat_buf;

	log_init();
	g_log_context.log_level = LOG_ERR;
	ignore_signal_pipe();

	if ((result=fdfs_client_init_ex(&tracker_group,g_conf_filename)) != 0)
	{
		return result;
	}

	pTrackerServer = tracker_get_connection_ex(&tracker_group);
	if (pTrackerServer == NULL)
	{
		fdfs_client_destroy_ex(&tracker_group);
		return errno != 0 ? errno : ECONNREFUSED;
	}

	*group_name = '\0';
	if ((result=tracker_query_storage_store(pTrackerServer, \
	                &storageServer, group_name, &store_path_index)) != 0)
	{
		fdfs_client_destroy();
		fprintf(stderr, "tracker_query_storage fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
		return result;
	}

	/*result = storage_upload_by_filename1(pTrackerServer, \
			&storageServer, store_path_index, \
			local_filename, NULL, \
			NULL, 0, group_name, file_id);*/
	if (stat(local_filename, &stat_buf) != 0)
	{
		group_name[0] = '\0';
		remote_filename[0] = '\0';
		return errno;
	}

	if (!S_ISREG(stat_buf.st_mode))
	{
		group_name[0] = '\0';
		remote_filename[0] = '\0';
		return EINVAL;
	}

	file_ext_name = fdfs_get_file_ext_name(local_filename);
	result = storage_do_upload_file(pTrackerServer,&storageServer,store_path_index,\
				STORAGE_PROTO_CMD_UPLOAD_FILE,FDFS_UPLOAD_BY_FILE,local_filename,\
				NULL, stat_buf.st_size, NULL, NULL, file_ext_name, \
			NULL, 0, group_name, remote_filename);	
	if (result == 0)
	{
		sprintf(file_id, "%s%c%s\n", group_name, \
			FDFS_FILE_ID_SEPERATOR, remote_filename);
		//printf("%s\n", file_id);
		fwrite(file_id,1,strlen(file_id),g_file_list_fp);
		fflush(g_file_list_fp);
	}
	else
	{
		fprintf(stderr, "upload file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
	}

	tracker_disconnect_server_ex(pTrackerServer, true);
	fdfs_client_destroy_ex(&tracker_group);

	return stat_buf.st_size;
}

static void *upload_thread(void *arg)
{
	//logDebug("thread_start\n");
	//fflush(stdout);
	int result;
	int for_warning;
	if((result = upload_file_by_filename(arg)) < 0)
	{
		printf("%df%d\n",result,__sync_add_and_fetch(&g_upload_test_info.fail_count,1));
		fflush(stdout);	
		if(__sync_add_and_fetch(&g_upload_test_info.all_count,1) == g_upload_test_info.upload_count)
			upload_calculate();
		return NULL;
	}
	for_warning = __sync_fetch_and_add(&g_upload_test_info.upload_size,result);
	printf("s%d\n",__sync_add_and_fetch(&g_upload_test_info.success_count,1));
	fflush(stdout);
	if(__sync_add_and_fetch(&g_upload_test_info.all_count,1) == g_upload_test_info.upload_count)
			upload_calculate();
	return NULL;
}

int upload_test(int file_num,int client_num)
{
	g_file_list_fp = fopen(default_file_list,"w+");

	g_upload_test_info.upload_count = file_num;
	thread_init(client_num);
	
	get_upload_file_names("temp");
	
	srand((unsigned int)time(NULL));
	
	g_upload_test_info.start_time = time(NULL);
	
	//int i;
	//for(i = 0;i != g_upload_test_info.name_counts;++i)
		//logDebug("%s\n",g_upload_test_info.file_names[i]);
	for(g_upload_test_info.have_upload_count = 0;g_upload_test_info.have_upload_count != file_num;++g_upload_test_info.have_upload_count)
	{
		//logDebug("add\n");
		//fflush(stdout);
		//logDebug("%s\n",g_upload_test_info.file_names[rand()%g_upload_test_info.name_counts]);
		pool_add_worker(upload_thread,g_upload_test_info.file_names[rand()%g_upload_test_info.name_counts]);
	}

	//logDebug("upload_test_calculate\n");
	//fflush(stdout);
	
	return 0;	
}

static void get_download_file_names(char *file_list)
{	
	FILE *fp;

	fp = fopen(file_list,"r");
	while(fgets(g_download_test_info.file_names[g_download_test_info.name_counts],128,fp) != NULL)
	{
		*strrchr(g_download_test_info.file_names[g_download_test_info.name_counts],'\n')\
											= '\0';
		g_download_test_info.name_counts++;
	}
	fclose(fp);
	return ;
}

static void download_calculate()
{
	//int i;
	//double real_sec = 0;
	g_download_test_info.all_sec = time(NULL)-g_download_test_info.start_time;
	//for(i = 0;i != g_download_test_info.all_count;++i)
	//	real_sec += g_download_test_info.every_sec[i];
	printf("download_size %lld,download_all_sec %d,download_real_sec %d,download_speed %fM/s\n"
		"return with control+C\n",\
		g_download_test_info.download_size,g_download_test_info.all_sec, \
		g_download_test_info.all_sec, \
		g_download_test_info.download_size/1048576/(double)g_download_test_info.all_sec);
	fflush(stdout);
	
	return ;
}

static int tcprecvdata_nb_ex_without_writing_buf(int sock, void *data, const int size, \
		const int timeout, int *count)
{
	int left_bytes;
	int read_bytes;
	int res;
	int ret_code;
	unsigned char* p;
#ifdef USE_SELECT
	fd_set read_set;
	struct timeval t;
#else
	struct pollfd pollfds;
#endif

#ifdef USE_SELECT
	FD_ZERO(&read_set);
	FD_SET(sock, &read_set);
#else
	pollfds.fd = sock;
	pollfds.events = POLLIN;
#endif

	read_bytes = 0;
	ret_code = 0;
	p = (unsigned char*)data;
	left_bytes = size;
	while (left_bytes > 0)
	{
		if(left_bytes < RECV_BUFF_SIZE)
		{
			read_bytes = recv(sock, p, left_bytes, 0);
			if (read_bytes > 0)
			{
				left_bytes -= read_bytes;
				p += read_bytes;
				continue;
			}
		}
		else
		{
			read_bytes = recv(sock, p, RECV_BUFF_SIZE, 0);
			if (read_bytes > 0)
			{
				left_bytes -= read_bytes;
				p += read_bytes;
				continue;
			}
		}

		if (read_bytes < 0)
		{

			if (!(errno == EAGAIN || errno == EWOULDBLOCK))
			{
				ret_code = errno != 0 ? errno : EINTR;
				break;
			}
		}
		else
		{
			ret_code = ENOTCONN;
			break;
		}

#ifdef USE_SELECT
		if (timeout <= 0)
		{
			res = select(sock+1, &read_set, NULL, NULL, NULL);
		}
		else
		{
			t.tv_usec = 0;
			t.tv_sec = timeout;
			res = select(sock+1, &read_set, NULL, NULL, &t);
		}
#else
		res = poll(&pollfds, 1, 1000 * timeout);
		if (pollfds.revents & POLLHUP)
		{
			ret_code = ENOTCONN;
			break;
		}
#endif

		if (res < 0)
		{
			ret_code = errno != 0 ? errno : EINTR;
			break;
		}
		else if (res == 0)
		{
			ret_code = ETIMEDOUT;
			break;
		}
	}

	if (count != NULL)
	{
		*count = size - left_bytes;
	}

	return ret_code;
}

static int fdfs_recv_response_without_writing_buf(ConnectionInfo *pTrackerServer, \
		char **buff, const int buff_size, \
		int64_t *in_bytes)
{
	int result;
	char temp[RECV_BUFF_SIZE];

	*buff = temp;

	result = fdfs_recv_header(pTrackerServer, in_bytes);
	if (result != 0)
	{
		return result;
	}

	if (*in_bytes == 0)
	{
		return 0;
	}

	if ((result=tcprecvdata_nb_ex_without_writing_buf(pTrackerServer->sock, *buff, \
		*in_bytes, g_fdfs_network_timeout,NULL)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server: %s:%d, recv data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
		*in_bytes = 0;
		return result;
	}

	return 0;
}

static int storage_get_connection(ConnectionInfo *pTrackerServer, \
		ConnectionInfo **ppStorageServer, const byte cmd, \
		const char *group_name, const char *filename, \
		ConnectionInfo *pNewStorage, bool *new_connection)
{
	int result;
	bool new_tracker_connection;
	ConnectionInfo *pNewTracker;
	if (*ppStorageServer == NULL)
	{
		CHECK_CONNECTION(pTrackerServer, pNewTracker, result, \
			new_tracker_connection);
		if (cmd == TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE)
		{
			result = tracker_query_storage_fetch(pNewTracker, \
		                pNewStorage, group_name, filename);
		}
		else
		{
			result = tracker_query_storage_update(pNewTracker, \
		                pNewStorage, group_name, filename);
		}

		if (new_tracker_connection)
		{
			tracker_disconnect_server_ex(pNewTracker, result != 0);
		}

		if (result != 0)
		{
			return result;
		}

		if ((*ppStorageServer=tracker_connect_server(pNewStorage, \
			&result)) == NULL)
		{
			return result;
		}

		*new_connection = true;
	}
	else
	{
		if ((*ppStorageServer)->sock >= 0)
		{
			*new_connection = false;
		}
		else
		{
			if ((*ppStorageServer=tracker_connect_server( \
				*ppStorageServer, &result)) == NULL)
			{
				return result;
			}

			*new_connection = true;
		}
	}

	return 0;
}

static int storage_do_download_file_ex_without_writing_disk(ConnectionInfo *pTrackerServer, \
		ConnectionInfo *pStorageServer, \
		const int download_type, \
		const char *group_name, const char *remote_filename, \
		const int64_t file_offset, const int64_t download_bytes, \
		char **file_buff, void *arg, int64_t *file_size)
{
	TrackerHeader *pHeader;
	int result;
	ConnectionInfo storageServer;
	char out_buff[sizeof(TrackerHeader)+FDFS_GROUP_NAME_MAX_LEN+128];
	char *p;
	int out_bytes;
	int64_t in_bytes;
	int filename_len;
	bool new_connection;

	*file_size = 0;
	if ((result=storage_get_connection(pTrackerServer, \
		&pStorageServer, TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE, \
		group_name, remote_filename, \
		&storageServer, &new_connection)) != 0)
	{
		return result;
	}
	do
	{
		memset(out_buff, 0, sizeof(out_buff));
		pHeader = (TrackerHeader *)out_buff;
		p = out_buff + sizeof(TrackerHeader);
		long2buff(file_offset, p);
		p += 8;
		long2buff(download_bytes, p);
		p += 8;
		snprintf(p, sizeof(out_buff) - (p - out_buff), "%s", group_name);
		p += FDFS_GROUP_NAME_MAX_LEN;
		filename_len = snprintf(p, sizeof(out_buff) - (p - out_buff), \
					"%s", remote_filename);
		p += filename_len;
		out_bytes = p - out_buff;
		long2buff(out_bytes - sizeof(TrackerHeader), pHeader->pkg_len);
		pHeader->cmd = STORAGE_PROTO_CMD_DOWNLOAD_FILE;
	
			
		if ((result=tcpsenddata_nb(pStorageServer->sock, out_buff, \
			out_bytes, g_fdfs_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"send data to storage server %s:%d fail, " \
				"errno: %d, error info: %s", __LINE__, \
				pStorageServer->ip_addr, pStorageServer->port, \
				result, STRERROR(result));
			break;
		}
	
		if (download_type == FDFS_DOWNLOAD_TO_BUFF)
		{
			*file_buff = NULL;
			if ((result=fdfs_recv_response_without_writing_buf(pStorageServer, \
				file_buff, 0, &in_bytes)) != 0)
			{
				break;
			}
		}
	
		*file_size = in_bytes;
	} while (0);

	if (new_connection)
	{
		tracker_disconnect_server_ex(pStorageServer, result != 0);
	}

	return result;
}

	#define FDFS_SPLIT_GROUP_NAME_AND_FILENAME(file_id) \
	char new_file_id[FDFS_GROUP_NAME_MAX_LEN + 128]; \
	char *group_name; \
	char *filename; \
	char *pSeperator; \
	\
	snprintf(new_file_id, sizeof(new_file_id), "%s", file_id); \
	pSeperator = strchr(new_file_id, FDFS_FILE_ID_SEPERATOR); \
	if (pSeperator == NULL) \
	{ \
		return EINVAL; \
	} \
	\
	*pSeperator = '\0'; \
	group_name = new_file_id; \
	filename =  pSeperator + 1; \

static int download_file_by_filename(char *file_name)
{
	char *local_filename;
	//char dir_filename[128];
	ConnectionInfo *pTrackerServer;
	int result;
	char file_id[128];
	int64_t file_size;
	int64_t file_offset;
	int64_t download_bytes;

	TrackerServerGroup tracker_group;
	
	
	memcpy(&tracker_group,&g_tracker_group,sizeof(tracker_group));
		tracker_group.servers = (ConnectionInfo *)malloc( \
		sizeof(ConnectionInfo) * tracker_group.server_count);
	

	if ((result=fdfs_load_tracker_group_ex(&tracker_group, \
			g_conf_filename, &g_iniContext)) != 0)
	{
		return result;
	}
	
	pTrackerServer = tracker_get_connection_ex(&tracker_group);
	if (pTrackerServer == NULL)
	{
		fdfs_client_destroy_ex(&tracker_group);
		return errno != 0 ? errno : ECONNREFUSED;
	}

	snprintf(file_id, sizeof(file_id), "%s", file_name);

	file_offset = 0;
	download_bytes = 0;
	{
		local_filename = strrchr(file_id, '/');
		if (local_filename != NULL)
		{
			local_filename++;  //skip /
		}
		else
		{
			local_filename = file_id;
		}
	}
	//sprintf(dir_filename,"download_temp/%s",local_filename);
	//local_filename = dir_filename;

/*
for writing disk
	result = storage_do_download_file1_ex(pTrackerServer, \
                NULL, FDFS_DOWNLOAD_TO_BUFF, file_id,\
                file_offset, download_bytes, \
                &local_filename, NULL, &file_size);
*/
/*without writing disk*/
	FDFS_SPLIT_GROUP_NAME_AND_FILENAME(file_id)

	result = storage_do_download_file_ex_without_writing_disk(pTrackerServer, \
		NULL, FDFS_DOWNLOAD_TO_BUFF, \
			group_name, filename, \
                file_offset, download_bytes, \
                &local_filename, NULL, &file_size);
/*without writing disk end*/
	if (result != 0)
	{
		printf("download file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
	}

	tracker_disconnect_server_ex(pTrackerServer, true);
	fdfs_client_destroy_ex(&tracker_group);

	return file_size;
}

ConnectionInfo* conn_pool_connect_server_test(ConnectionInfo *pConnection)
{
	//int result;

	if (pConnection->sock >= 0)
	{
		close(pConnection->sock);
	}

	pConnection->sock = socket(AF_INET, SOCK_STREAM, 0);
	if(pConnection->sock < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"socket create failed, errno: %d, " \
			"error info: %s", __LINE__, errno, STRERROR(errno));
		return NULL;
	}
/*	
	if ((result=tcpsetnonblockopt(pConnection->sock)) != 0)
	{
		close(pConnection->sock);
		pConnection->sock = -1;
		return NULL;
	}
*/
/*
	if ((result=connectserverbyip_nb(pConnection->sock, \
		pConnection->ip_addr, pConnection->port, \
		g_fdfs_connect_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"connect to %s:%d fail, errno: %d, " \
			"error info: %s", __LINE__, pConnection->ip_addr, \
			pConnection->port, result, STRERROR(result));

		close(pConnection->sock);
		pConnection->sock = -1;
		return NULL;
	}
*/
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(pConnection->port);
	if(inet_aton(pConnection->ip_addr, &addr.sin_addr) != 1)
	{
		logError(	"file: "__FILE__",line :%d"\
			"connect_ip call inet_pton failed,"\
			"errno :%d,errno info: %s",\
			__LINE__,errno,strerror(errno));
		return NULL;
	}

	do
	{
		if(connect(pConnection->sock,(struct sockaddr*)&addr,sizeof(addr)) < 0)
		{
			if(errno == EAGAIN)
				continue ;
			logError(	"file :"__FILE__",line :%d"\
				"connect_ip %s:%d call connect failed,"\
				"errno :%d,errno info: %s",\
				__LINE__,pConnection->ip_addr,pConnection->port,errno,strerror(errno));
			close(pConnection->sock);
			pConnection->sock = -1;
			return NULL;
		}
		else
			break;
	}while(1);
	return pConnection;
}

static int fdfs_recv_header_test(ConnectionInfo *pTrackerServer, int64_t *in_bytes)
{
	TrackerHeader resp;
	int result;

	if ((result=tcprecvdata_nb(pTrackerServer->sock, &resp, \
		sizeof(resp), g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, recv data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
		*in_bytes = 0;
		return result;
	}

	if (resp.status != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, response status %d != 0", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, resp.status);

		*in_bytes = 0;
		return resp.status;
	}

	*in_bytes = buff2long(resp.pkg_len);
	if (*in_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, recv package size " \
			INT64_PRINTF_FORMAT" is not correct", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, *in_bytes);
		*in_bytes = 0;
		return EINVAL;
	}

	return resp.status;
}

static int fdfs_recv_response_test(ConnectionInfo *pTrackerServer, \
		char **buff, const int buff_size, \
		int64_t *in_bytes)
{
	int result;
	bool bMalloced;

	result = fdfs_recv_header_test(pTrackerServer, in_bytes);
	if (result != 0)
	{
		return result;
	}

	if (*in_bytes == 0)
	{
		return 0;
	}

	if (*buff == NULL)
	{
		*buff = (char *)malloc((*in_bytes) + 1);
		if (*buff == NULL)
		{
			*in_bytes = 0;

			logError("file: "__FILE__", line: %d, " \
				"malloc "INT64_PRINTF_FORMAT" bytes fail", \
				__LINE__, (*in_bytes) + 1);
			return errno != 0 ? errno : ENOMEM;
		}

		bMalloced = true;
	}
	else 
	{
		if (*in_bytes > buff_size)
		{
			logError("file: "__FILE__", line: %d, " \
				"server: %s:%d, recv body bytes: " \
				INT64_PRINTF_FORMAT" exceed max: %d", \
				__LINE__, pTrackerServer->ip_addr, \
				pTrackerServer->port, *in_bytes, buff_size);
			*in_bytes = 0;
			return ENOSPC;
		}

		bMalloced = false;
	}

	if ((result=tcprecvdata_nb(pTrackerServer->sock, *buff, \
		*in_bytes, g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server: %s:%d, recv data fail, " \
			"errno: %d, error info: %s", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
		*in_bytes = 0;
		if (bMalloced)
		{
			free(*buff);
			*buff = NULL;
		}
		return result;
	}

	return 0;
}

static int storage_get_connection_test(ConnectionInfo *pTrackerServer, \
		ConnectionInfo **ppStorageServer, const byte cmd, \
		const char *group_name, const char *filename, \
		ConnectionInfo *pNewStorage, bool *new_connection)
{
	int result;

	ConnectionInfo *pStorageServer = pNewStorage;
	TrackerHeader *pHeader;
	ConnectionInfo *conn;
	char out_buff[sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 128];
	char in_buff[sizeof(TrackerHeader) + TRACKER_QUERY_STORAGE_FETCH_BODY_LEN];
	char *pInBuff;
	int64_t in_bytes;
	int filename_len;

	conn = pTrackerServer;
	memset(pStorageServer, 0, sizeof(ConnectionInfo));
		pStorageServer->sock = -1;

	memset(out_buff, 0, sizeof(out_buff));
	pHeader = (TrackerHeader *)out_buff;
	snprintf(out_buff + sizeof(TrackerHeader), sizeof(out_buff) - \
		sizeof(TrackerHeader),  "%s", group_name);
		filename_len = snprintf(out_buff + sizeof(TrackerHeader) + \
		FDFS_GROUP_NAME_MAX_LEN, \
		sizeof(out_buff) - sizeof(TrackerHeader) - \
		FDFS_GROUP_NAME_MAX_LEN,  "%s", filename);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN + filename_len, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE;
	if ((result=tcpsenddata_nb(conn->sock, out_buff, \
		sizeof(TrackerHeader) + FDFS_GROUP_NAME_MAX_LEN + 
		filename_len, g_fdfs_network_timeout)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"send data to tracker server %s:%d fail, " \
			"errno: %d, error info: %s", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, \
			result, STRERROR(result));
	}
	else
	{
		pInBuff = in_buff;
		result = fdfs_recv_response_test(conn, \
			&pInBuff, sizeof(in_buff), &in_bytes);
	}

	if (result != 0)
	{
		return result;
	}

	if (in_bytes != TRACKER_QUERY_STORAGE_FETCH_BODY_LEN)
	{
		logError("file: "__FILE__", line: %d, " \
			"tracker server %s:%d response data " \
			"length: "INT64_PRINTF_FORMAT" is invalid, " \
			"expect length: %d", __LINE__, \
			pTrackerServer->ip_addr, \
			pTrackerServer->port, in_bytes, \
			TRACKER_QUERY_STORAGE_FETCH_BODY_LEN);
		return EINVAL;
	}

	memcpy(pStorageServer->ip_addr, in_buff + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE-1);
	pStorageServer->port = (int)buff2long(in_buff + \
			FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);

	if (result != 0)
	{
		return result;
	}

	if ((*ppStorageServer=conn_pool_connect_server_test(pNewStorage))\
		== NULL)
	{
		return result;
	}

	if (result != 0)
	{
		printf("download file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
	}
	return 0;
}

int download_by_filename(char *file_name)
{
	char *local_filename;
	ConnectionInfo *pTrackerServer;
	int result;
	char file_id[128];
	int64_t file_size;
	int64_t file_offset;
	int64_t download_bytes;

	TrackerServerGroup tracker_group;
	TrackerServerGroup *pTrackerGroup = &tracker_group;
	
	memcpy(&tracker_group,&g_tracker_group,sizeof(tracker_group));
		tracker_group.servers = (ConnectionInfo *)malloc( \
		sizeof(ConnectionInfo) * tracker_group.server_count);

	if ((result=fdfs_load_tracker_group_ex(&tracker_group, \
			g_conf_filename, &g_iniContext)) != 0)
	{
		return result;
	}
	ConnectionInfo *pCurrentServer;
	ConnectionInfo *pServer;
	ConnectionInfo *pEnd;
	int server_index;

	server_index = pTrackerGroup->server_index;
	if (server_index >= pTrackerGroup->server_count)
	{
		server_index = 0;
	}

	do
	{
	pCurrentServer = pTrackerGroup->servers + server_index;
	if ((pTrackerServer=conn_pool_connect_server_test(pCurrentServer)) != NULL)
	{
		break;
	}

	pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
	for (pServer=pCurrentServer+1; pServer<pEnd; pServer++)
	{
		if ((pTrackerServer=conn_pool_connect_server_test(pServer)) != NULL)
		{
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}

	if (pTrackerServer != NULL)
	{
		break;
	}

	for (pServer=pTrackerGroup->servers; pServer<pCurrentServer; pServer++)
	{
		if ((pTrackerServer=conn_pool_connect_server_test(pServer)) != NULL)
		{
			pTrackerGroup->server_index = pServer - \
							pTrackerGroup->servers;
			break;
		}
	}
	} while (0);

	pTrackerGroup->server_index++;
	if (pTrackerGroup->server_index >= pTrackerGroup->server_count)
	{
		pTrackerGroup->server_index = 0;
	}

	snprintf(file_id, sizeof(file_id), "%s", file_name);

	file_offset = 0;
	download_bytes = 0;
	{
		local_filename = strrchr(file_id, '/');
		if (local_filename != NULL)
		{
			local_filename++;  //skip /
		}
		else
		{
			local_filename = file_id;
		}
	}
	
	FDFS_SPLIT_GROUP_NAME_AND_FILENAME(file_id)
	
	ConnectionInfo *pStorageServer = NULL;
	const int download_type = FDFS_DOWNLOAD_TO_BUFF;
	const char* remote_filename = filename;
	char **file_buff = &local_filename;
	TrackerHeader *pHeader;
	ConnectionInfo storageServer;
	char out_buff[sizeof(TrackerHeader)+FDFS_GROUP_NAME_MAX_LEN+128];
	char *p;
	int out_bytes;
	int64_t in_bytes;
	int filename_len;
	bool new_connection;

	file_size = 0;
	if ((result=storage_get_connection_test(pTrackerServer, \
		&pStorageServer, TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE, \
		group_name, remote_filename, \
		&storageServer, &new_connection)) != 0)
	{
		return result;
	}

	do
	{
		memset(out_buff, 0, sizeof(out_buff));
		pHeader = (TrackerHeader *)out_buff;
		p = out_buff + sizeof(TrackerHeader);
		long2buff(file_offset, p);
		p += 8;
		long2buff(download_bytes, p);
		p += 8;
		snprintf(p, sizeof(out_buff) - (p - out_buff), "%s", group_name);
		p += FDFS_GROUP_NAME_MAX_LEN;
		filename_len = snprintf(p, sizeof(out_buff) - (p - out_buff), \
					"%s", remote_filename);
		p += filename_len;
		out_bytes = p - out_buff;
		long2buff(out_bytes - sizeof(TrackerHeader), pHeader->pkg_len);
		pHeader->cmd = STORAGE_PROTO_CMD_DOWNLOAD_FILE;
	
		
		/*printf("%s\n",group_name);
		printf("%d\n",out_bytes);
		printf("%d\n",filename_len);
		int i;
		for(i = 0;i != out_bytes;++i)
			printf("%c",out_buff[i]+'0');
		printf("\n");*/

		if ((result=tcpsenddata_nb(pStorageServer->sock, out_buff, \
			out_bytes, g_fdfs_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"send data to storage server %s:%d fail, " \
				"errno: %d, error info: %s", __LINE__, \
				pStorageServer->ip_addr, pStorageServer->port, \
				result, STRERROR(result));
			break;
		}
	
		if (download_type == FDFS_DOWNLOAD_TO_BUFF)
		{
			*file_buff = NULL;
			if ((result=fdfs_recv_response_test(pStorageServer, \
				file_buff, 0, &in_bytes)) != 0)
			{
				break;
			}
		}
	
		file_size = in_bytes;
	} while (0);

	if (result != 0)
	{
		printf("download file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
	}
	tracker_disconnect_server_ex(pStorageServer, result != 0);
	
	tracker_disconnect_server_ex(pTrackerServer, true);
	fdfs_client_destroy_ex(&tracker_group);
	
	return file_size;

}
static void *download_thread(void *arg)
{
	//logDebug("thread_start\n");
	//fflush(stdout);
	int result;
	int for_warning;
		
	/*struct rusage usage;
	struct timeval u_start,s_start;	
	getrusage(RUSAGE_THREAD,&usage);
	memcpy(&u_start,&usage.ru_utime,sizeof(u_start));
	memcpy(&s_start,&usage.ru_stime,sizeof(s_start));
	*/
	if((result = download_by_filename(arg)) < 1)
	{
		printf("%df%d\n",result,__sync_add_and_fetch(&g_download_test_info.fail_count,1));
		if(__sync_add_and_fetch(&g_download_test_info.all_count,1) == g_download_test_info.name_counts)
			download_calculate();
		fflush(stdout);	
		return NULL;
	}
	/*
	getrusage(RUSAGE_THREAD,&usage);
	g_download_test_info.every_sec[g_download_test_info.all_count] = \
		usage.ru_utime.tv_sec - u_start.tv_sec + ((double)(usage.ru_utime.tv_usec - u_start.tv_usec))/1000 + \
		usage.ru_stime.tv_sec - s_start.tv_sec  + ((double)(usage.ru_stime.tv_usec - s_start.tv_usec))/1000;
	//printf("%f",g_download_test_info.every_sec[g_download_test_info.all_count]);
	*/
	for_warning = __sync_fetch_and_add(&g_download_test_info.download_size,result);
	printf("s%d\n",__sync_add_and_fetch(&g_download_test_info.success_count,1));
	fflush(stdout);
	if(__sync_add_and_fetch(&g_download_test_info.all_count,1) == g_download_test_info.name_counts)
		download_calculate();
	
	//sleep(g_wait_for_TIMEWAIT_sec);
	return NULL;
}

static int storage_get_connection_test_nb(ConnectionInfo *pTrackerServer, \
		ConnectionInfo **ppStorageServer, const byte cmd, \
		const char *group_name, const char *filename, \
		ConnectionInfo *pNewStorage, bool *new_connection, \
		download_test_file_info_t *pTracker)
{
	//printf("%p:storage_get_connection_test_nb\n",pTracker);fflush(stdout);
	//int result;
	
	TrackerHeader *pHeader;
	int64_t in_bytes = 0;

	memset(&pTracker->pNewStorage, 0, sizeof(ConnectionInfo));
		pTracker->pNewStorage.sock = -1;

	memset(pTracker->out_buff, 0, sizeof(pTracker->out_buff));
	pHeader = (TrackerHeader *)pTracker->out_buff;
	snprintf(pTracker->out_buff + sizeof(TrackerHeader), sizeof(pTracker->out_buff) - \
		sizeof(TrackerHeader),  "%s", group_name);
		pTracker->filename_len = snprintf(pTracker->out_buff + sizeof(TrackerHeader) + \
		FDFS_GROUP_NAME_MAX_LEN, \
		sizeof(pTracker->out_buff) - sizeof(TrackerHeader) - \
		FDFS_GROUP_NAME_MAX_LEN,  "%s", filename);
	
	long2buff(FDFS_GROUP_NAME_MAX_LEN + pTracker->filename_len, pHeader->pkg_len);
	pHeader->cmd = TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE;
	
	pTracker->data.buff = pTracker->out_buff;
	pTracker->data.need_size = sizeof(TrackerHeader) +\
		FDFS_GROUP_NAME_MAX_LEN + pTracker->filename_len;
	pTracker->data.total_size = 0;
	pTracker->data.proc = storage_get_connection_test_nb2;
	pTracker->data.final_proc = download_by_filename_nb2;
	pTracker->get_connection_to_storage = true;
	
	//printf("%p:need_size:%d,fd:%d\n",pTracker,pTracker->data.need_size,conn->sock);
	pTracker->conn = pTracker->TrackerServer;
	pTracker->in_bytes = in_bytes;
	pTracker->remote_filename = filename;
	if(aeCreateFileEvent(pTracker->eventLoop, pTracker->conn->sock, AE_WRITABLE,nb_sock_send_data,pTracker) != AE_OK)
	{
		logError(	"file :"__FILE__",line :%d"\
			"create_tracker_service CreateFileEvent failed.");
			return -4;
	}
	return 0;
}	

static void storage_get_connection_test_nb2(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	//char *pInBuff;

	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;
	
	//printf("%p:storage_get_connection_test_nb2\n",pTracker);fflush(stdout);
	pTracker->data.buff = &pTracker->resp;
	pTracker->data.need_size = sizeof(pTracker->resp);
	pTracker->data.total_size = 0;
	pTracker->data.proc = storage_get_connection_test_nb3;
	
	if(aeCreateFileEvent(eventLoop, pTracker->conn->sock, AE_READABLE,nb_sock_recv_data,pTracker) != AE_OK)
	{
		logError(	"file :"__FILE__",line :%d"\
			"create_tracker_service CreateFileEvent failed.");
			return ;
	}
	return ;
}

static void storage_get_connection_test_nb3(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	TrackerHeader resp;
	//int result;

	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;
	ConnectionInfo *pTrackerServer = pTracker->conn;

	//printf("%p:storage_get_connection_test_nb3\n",pTracker);fflush(stdout);
	int buff_size;
	if(pTracker->get_connection_to_storage)
		buff_size = sizeof(pTracker->in_buff);
	else buff_size = RECV_BUFF_SIZE;
	
	resp = pTracker->resp;
	//printf("%ld\n",buff2long(resp.pkg_len));
	if (resp.status != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, response status %d != 0", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, resp.status);
		pTracker->in_bytes = 0;
		//return resp.status;
		return ;
	}

	pTracker->in_bytes = buff2long(resp.pkg_len);
	if (pTracker->in_bytes < 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, recv package size " \
			INT64_PRINTF_FORMAT" is not correct", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, pTracker->in_bytes);
		pTracker->in_bytes = 0;
		//return EINVAL;
		return ;
	}

	//return resp.status;
	
	/*if (result != 0)
	{
		return result;
	}

	if (*in_bytes == 0)
	{
		return 0;
	}*/

	if(pTracker->get_connection_to_storage)
	if (pTracker->in_bytes > buff_size)
	{
		logError("file: "__FILE__", line: %d, " \
			"server: %s:%d, recv body bytes: " \
			INT64_PRINTF_FORMAT" exceed max: %d", \
			__LINE__, pTrackerServer->ip_addr, \
			pTrackerServer->port, pTracker->in_bytes, buff_size);
		pTracker->in_bytes = 0;
	//	return ENOSPC;
		return ;
	}

	//printf("%ld\n",pTracker->in_bytes);
	if(pTracker->get_connection_to_storage)
		pTracker->data.buff = pTracker->in_buff;
	else pTracker->data.buff = pTracker->temp;
	pTracker->data.need_size = pTracker->in_bytes;
	pTracker->data.total_size = 0;
	pTracker->data.proc = storage_get_connection_test_nb4;
	
	if(pTracker->get_connection_to_storage)
	{
		if(aeCreateFileEvent(eventLoop, pTracker->conn->sock, AE_READABLE,nb_sock_recv_data,pTracker) != AE_OK)
		{
			logError(	"file :"__FILE__",line :%d"\
				"create_tracker_service CreateFileEvent failed.");
				return ;
		}
	}
	else
	{
		if(aeCreateFileEvent(eventLoop, pTracker->conn->sock, AE_READABLE,nb_sock_recv_data_for_test,pTracker) != AE_OK)
		{
			logError(	"file :"__FILE__",line :%d"\
				"create_tracker_service CreateFileEvent failed.");
				return ;
		}
	}
	return ;
}
		
	
static void storage_get_connection_test_nb4(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	
	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;
	ConnectionInfo *pTrackerServer = pTracker->conn;
	int in_bytes = pTracker->in_bytes;
	
	//printf("%p:storage_get_connection_test_nb4\n",pTracker);fflush(stdout);
	

	if(pTracker->get_connection_to_storage)
	{
		if (in_bytes != TRACKER_QUERY_STORAGE_FETCH_BODY_LEN)
		{
			logError("file: "__FILE__", line: %d, " \
				"tracker server %s:%d response data " \
				"length: "INT64_PRINTF_FORMAT" is invalid, " \
				"expect length: %d", __LINE__, \
				pTrackerServer->ip_addr, \
				pTrackerServer->port, in_bytes, \
				TRACKER_QUERY_STORAGE_FETCH_BODY_LEN);
			//return EINVAL;
			return ;
		}
	

		memcpy(pTracker->pNewStorage.ip_addr, pTracker->in_buff + \
			FDFS_GROUP_NAME_MAX_LEN, IP_ADDRESS_SIZE-1);
		pTracker->pNewStorage.port = (int)buff2long(pTracker->in_buff + \
				FDFS_GROUP_NAME_MAX_LEN + IP_ADDRESS_SIZE - 1);

	/*printf("%s:%d",pTracker->pNewStorage.ip_addr, \
			pTracker->pNewStorage.port);
	fflush(stdout);*/
	/*if (result != 0)
	{
		return result;
	}*/

		if (conn_pool_connect_server_test(&pTracker->pNewStorage)\
			== NULL)
		{
			tracker_disconnect_server_ex(pTracker->TrackerServer, true);
			fdfs_client_destroy_ex(&pTracker->tracker_group);
			pTracker->in_bytes = -1;
			download_by_filename_final(eventLoop,0,pTracker,0);
			//return result;
			return ;
		}

		//printf("\n%d,%d\n",pTracker->pNewStorage.sock,pTracker->TrackerServer.sock);
		/*if (result != 0)
		{
			printf("download file fail, " \
				"error no: %d, error info: %s\n", \
				result, STRERROR(result));
		}*/
		//return 0;
	}
	pTracker->data.final_proc(eventLoop,0,pTracker,0);
	//download_by_filename_nb2(pTracker);
	return ;
}

int download_by_filename_nb(download_test_file_info_t *pTracker,char *file_name,aeEventLoop *eventLoop)
{
	//printf("%p:download_by_filename_nb\n",pTracker);fflush(stdout);
	char *local_filename;
	ConnectionInfo **pTrackerServer;
	TrackerServerGroup *pTrackerGroup;
	int result;
	char file_id[128];
//	int64_t file_size;
//	int64_t file_offset;

	if(pTracker == NULL)
	{
		pTracker = malloc(sizeof(*pTracker));
	}

	memset(pTracker,0,sizeof(*pTracker));
	pTrackerServer = &pTracker->TrackerServer;
	pTrackerGroup = &pTracker->tracker_group;
	pTracker->eventLoop = eventLoop;
	
	memcpy(pTrackerGroup,&g_tracker_group,sizeof(*pTrackerGroup));
	{
		pTrackerGroup->servers = pTracker->servers;
		/*(ConnectionInfo *)malloc( \
		sizeof(ConnectionInfo) * pTrackerGroup->server_count);*/
	}

	if ((result=fdfs_load_tracker_group_ex(pTrackerGroup, \
			g_conf_filename, &g_iniContext)) != 0)
	{
		return result;
	}
	ConnectionInfo *pCurrentServer;
	ConnectionInfo *pServer;
	ConnectionInfo *pEnd;
	int server_index;
	
	server_index = pTrackerGroup->server_index;
	if (server_index >= pTrackerGroup->server_count)
	{
		server_index = 0;
	}

	do
	{
		pCurrentServer = pTrackerGroup->servers + server_index;
		if ((*pTrackerServer=conn_pool_connect_server_test(pCurrentServer)) != NULL)
		{
			break;
		}
		pEnd = pTrackerGroup->servers + pTrackerGroup->server_count;
		for (pServer=pCurrentServer+1; pServer<pEnd; pServer++)
		{
			if ((*pTrackerServer=conn_pool_connect_server_test(pServer)) != NULL)
			{
				pTrackerGroup->server_index = pServer - \
								pTrackerGroup->servers;
				break;
			}
		}
		if (*pTrackerServer != NULL)
		{
			break;
		}
		for (pServer=pTrackerGroup->servers; pServer<pCurrentServer; pServer++)
		{
			if ((*pTrackerServer=conn_pool_connect_server_test(pServer)) != NULL)
			{
				pTrackerGroup->server_index = pServer - \
								pTrackerGroup->servers;
				break;
			}
		}
	} while (0);

	if(*pTrackerServer == NULL)
	{
		pTracker->in_bytes = -1;
		download_by_filename_final(eventLoop,0,pTracker,0);
		return -1;
	}
	pTrackerGroup->server_index++;
	if (pTrackerGroup->server_index >= pTrackerGroup->server_count)
	{
		pTrackerGroup->server_index = 0;
	}
	
	//printf("%p:fd %d,fd %d\n",pTracker,pTrackerServer->sock,pTracker->TrackerServer.sock);
	snprintf(file_id, sizeof(file_id), "%s", file_name);

//	file_offset = 0;
	{
		local_filename = strrchr(file_id, '/');
		if (local_filename != NULL)
		{
			local_filename++;  //skip /
		}
		else
		{
			local_filename = file_id;
		}
	}
	
	FDFS_SPLIT_GROUP_NAME_AND_FILENAME(file_id)
	memcpy(pTracker->group_name,group_name,FDFS_GROUP_NAME_MAX_LEN + 128);
	
	//const int download_type = FDFS_DOWNLOAD_TO_BUFF;
	pTracker->remote_filename = pTracker->group_name + (filename - group_name);
	//char **file_buff = &local_filename;

	//file_size = 0;
	if ((result=storage_get_connection_test_nb(*pTrackerServer, \
		NULL, TRACKER_PROTO_CMD_SERVICE_QUERY_FETCH_ONE, \
		group_name, pTracker->remote_filename, \
		NULL, NULL, pTracker)) != 0)
	{
		return result;
	}
	return 0;
}

static void download_by_filename_nb2(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	//sleep(1);
	//printf("%p,download_by_filename_nb2\n",clientData);fflush(stdout);
	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;
	
	char *p;
	TrackerHeader *pHeader = pTracker->pHeader;
	int64_t download_bytes = 0;

	memset(pTracker->out_buff, 0, sizeof(pTracker->out_buff));
	pHeader = (TrackerHeader *)pTracker->out_buff;
	p = pTracker->out_buff + sizeof(TrackerHeader);
	long2buff(pTracker->file_offset, p);
	p += 8;
	long2buff(download_bytes, p);
	p += 8;
	snprintf(p, sizeof(pTracker->out_buff) - (p - pTracker->out_buff), "%s", pTracker->group_name);
	p += FDFS_GROUP_NAME_MAX_LEN;
	pTracker->filename_len = snprintf(p, sizeof(pTracker->out_buff) - (p - pTracker->out_buff), \
				"%s", pTracker->remote_filename);
	p += pTracker->filename_len;
	pTracker->out_bytes = p - pTracker->out_buff;
	long2buff(pTracker->out_bytes - sizeof(TrackerHeader), pHeader->pkg_len);
	pHeader->cmd = STORAGE_PROTO_CMD_DOWNLOAD_FILE;

	/*printf("%s\n",pTracker->group_name);
	printf("%d\n",pTracker->out_bytes);
	printf("%d\n",pTracker->filename_len);
	int i;
	for(i = 0;i != pTracker->out_bytes;++i)
		printf("%c",pTracker->out_buff[i]+'0');
	printf("\n");*/
	pTracker->data.buff = pTracker->out_buff;
	pTracker->data.need_size = pTracker->out_bytes;
	pTracker->data.total_size = 0;
	pTracker->data.proc = download_by_filename_nb3;
	
	if(aeCreateFileEvent(eventLoop, pTracker->pNewStorage.sock, AE_WRITABLE,nb_sock_send_data,pTracker) != AE_OK)
	{
		logError(	"file :"__FILE__",line :%d"\
			"create_tracker_service CreateFileEvent failed.");
			return ;
	}
}

static void download_by_filename_nb3(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	//printf("%p,download_by_filename_nb3\n",clientData);fflush(stdout);
	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;
	/*
	result = fdfs_recv_response_test(conn, \
			&pInBuff, sizeof(in_buff), &in_bytes);
	*file_buff = NULL;
	if ((result=fdfs_recv_response_test(pStorageServer,\
		file_buff, 0, &in_bytes)) != 0)
	{
		break;
	}*/
	
	memset(pTracker->in_buff,0,sizeof(pTracker->in_buff));
	pTracker->data.buff = &pTracker->resp;
	pTracker->data.need_size = sizeof(pTracker->resp);
	pTracker->data.total_size = 0;
	pTracker->data.proc = storage_get_connection_test_nb3;
	pTracker->data.final_proc = download_by_filename_final;
	pTracker->get_connection_to_storage = false;
	
	pTracker->conn = &pTracker->pNewStorage;
	if(aeCreateFileEvent(eventLoop, pTracker->conn->sock, AE_READABLE,nb_sock_recv_data,pTracker) != AE_OK)
	{
		logError(	"file :"__FILE__",line :%d"\
			"create_tracker_service CreateFileEvent failed.");
			return ;
	}
}

static void download_by_filename_final(struct aeEventLoop *eventLoop, int sockfd, void *clientData, int mask)
{
	//printf("%p:download_by_filename_nb4\n",clientData);fflush(stdout);
	download_test_file_info_t *pTracker;
	pTracker = (download_test_file_info_t*)clientData;

	int file_size = pTracker->in_bytes;
	int success_count = 0;
	int for_warning;

	if(file_size == -1)
	{
		for_warning = __sync_add_and_fetch(&g_download_test_info.fail_count,1);
		if(__sync_add_and_fetch(&g_download_test_info.all_count,0) >= g_download_test_info.name_counts)
				return ;
		else
		{
			pTracker->download_filename = g_download_test_info.file_names[__sync_add_and_fetch(&g_download_test_info.all_count,1)];
			download_by_filename_nb(pTracker,pTracker->download_filename,eventLoop);
		}
		return ;
	}
	//printf("%d\n",file_size);
	/*if (result != 0)
	{
		printf("download file fail, " \
			"error no: %d, error info: %s\n", \
			result, STRERROR(result));
	}*/
	tracker_disconnect_server_ex(&pTracker->pNewStorage, true);// result != 0);
	
	tracker_disconnect_server_ex(pTracker->TrackerServer, true);
	fdfs_client_destroy_ex(&pTracker->tracker_group);
	
	for_warning = __sync_fetch_and_add(&g_download_test_info.download_size,file_size);
	success_count = __sync_add_and_fetch(&g_download_test_info.success_count,1);
	//if(success_count % 100 == 0)
	{
		printf("s%d,f%d\n",success_count,__sync_add_and_fetch(&g_download_test_info.fail_count,0));
		fflush(stdout);
	}
	if(success_count + __sync_add_and_fetch(&g_download_test_info.fail_count,0) == g_download_test_info.name_counts - 1)
		download_calculate();
	else 
		{
			if(__sync_add_and_fetch(&g_download_test_info.all_count,0) >= g_download_test_info.name_counts)
				return ;
			else
			{
				pTracker->download_filename = g_download_test_info.file_names[__sync_add_and_fetch(&g_download_test_info.all_count,1)];
				download_by_filename_nb(pTracker,pTracker->download_filename,eventLoop);
			}
		}
	//return file_size;
	return ;
}

int download_test(char *file_list,int client_num)
{
	get_download_file_names(file_list);

	thread_init(client_num);
	
	/*if(mkdir("download_temp",0777) < 0)
	{
		fprintf(stderr,"mkdir error!");
		fflush(stderr);
		return -1;
	}
	*/
	/*in order not rand*/
	//srand((unsigned int)time(NULL));
	
	g_download_test_info.start_time = time(NULL);

	for(g_download_test_info.have_download_count = 0;g_download_test_info.have_download_count != g_download_test_info.name_counts;++g_download_test_info.have_download_count)
	{
		pool_add_worker(download_thread,g_download_test_info.file_names[g_download_test_info.have_download_count]);
	}
	
	return 0;	
}

static void *epoll_download_thread(void *arg)
{
	aeEventLoop *eventLoop = (aeEventLoop*)arg;
	
	aeSetBeforeSleepProc(eventLoop, NULL);
	aeMain(eventLoop);
	aeDeleteEventLoop(eventLoop);
	
	return NULL;
}

int download_by_epoll(char *file_list,int client_num)
{
	int i;
	get_download_file_names(file_list);

	g_download_test_info_nb.eventLoopCounts = 8;

	for(i = 0; i != g_download_test_info_nb.eventLoopCounts; ++i)
	{
		if((g_download_test_info_nb.eventLoop[i] = (aeEventLoop*)aeCreateEventLoop()) == NULL)
		{
			logError("file :"__FILE__",line :%d"\
					"create_tracker_service CreateEventLoop failed.");
				return -1;
		}
	}
	for(i = 0; i != client_num; ++i)
	{
		download_by_filename_nb(NULL,g_download_test_info.file_names[i],g_download_test_info_nb.eventLoop[(i % g_download_test_info_nb.eventLoopCounts)]);
	//	printf("%p\n",g_download_test_info_nb.eventLoop[(i % g_download_test_info_nb.eventLoopCounts)]);
	}
	g_download_test_info.all_count += client_num;
	g_download_test_info.start_time = time(NULL);
	for(i = 0; i != g_download_test_info_nb.eventLoopCounts; ++i)
	{
		pthread_create(g_download_test_info_nb.thread + i, NULL, epoll_download_thread, g_download_test_info_nb.eventLoop[i]);
	}
	return 0;
}

static void usage(char *argv[])
{
	printf("Usage: %s <config_file> create <file_num> <file_size> " \
		"\n", argv[0]);
	printf("Usage: %s <config_file> upload <file_num> <client_num>" \
		"\n", argv[0]);
	printf("Usage: %s <config_file> download <file_list> <client_num> [wait_for_timewait_sec]" \
		"\n", argv[0]);
}


int main(int argc,char *argv[])
{
	int file_num,file_size,client_num;
	int result;
	if (argc < 3)
	{
		usage(argv);
		return 1;
	}
	g_conf_filename = argv[1];

	if(argc == 6)
	{
		g_wait_for_TIMEWAIT_sec = atoi(argv[5]);
	}
/*fdfs_client_init_conf_file*/
	log_init();
	if(access("error.log",0) != 0)
		g_log_context.log_fd = open("error.log", O_CREAT | O_WRONLY | O_APPEND, 0777);
	else 
		g_log_context.log_fd = open("error.log", O_WRONLY | O_APPEND);
	g_log_context.log_level = LOG_ERR;
	ignore_signal_pipe();
	
	if ((result = iniLoadFromFile(g_conf_filename, &g_iniContext)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"load conf file \"%s\" fail, ret code: %d", \
			__LINE__, g_conf_filename, result);
		return -1;
	}
	fdfs_client_do_init_ex(&g_tracker_group, g_conf_filename, \
				&g_iniContext);
/*fdfs_client_init_conf_file_end*/

	if(strcmp(argv[2],"create") == 0)
	{
		file_num = atoi(argv[3]);
		file_size = atoi(argv[4]);
		
		if(create_test_files(file_num,file_size) < 0)
			return -2;
	}
	
	if(strcmp(argv[2],"upload") == 0)
	{
		file_num = atoi(argv[3]);
		client_num = atoi(argv[4]);
		if(upload_test(file_num,client_num) < 0)
			return -3;
		sleep(100000000);
		return 0;
	}
	
	if(strcmp(argv[2],"download") == 0)
	{
		client_num = atoi(argv[4]);
		if(download_test(argv[3],client_num) < 0)
			return -4;
		sleep(100000000);
		return 0;
	}
	
	if(strcmp(argv[2],"download_by_epoll_foronefile") == 0)
	{
		if((g_download_test_info_nb.eventLoop[0] = (aeEventLoop*)aeCreateEventLoop()) == NULL)
		{
			logError(	"file :"__FILE__",line :%d"\
				"create_tracker_service CreateEventLoop failed.");
				return -3;
		}
		//client_num = atoi(argv[4]);
		if(download_by_filename_nb(NULL,argv[3],g_download_test_info_nb.eventLoop[0]) < 0)
			return -5;
		aeSetBeforeSleepProc(g_download_test_info_nb.eventLoop[0],NULL);
		aeMain(g_download_test_info_nb.eventLoop[0]);
		aeDeleteEventLoop(g_download_test_info_nb.eventLoop[0]);
		sleep(100000000);	
		return 0;
	}
	if(strcmp(argv[2],"download_by_epoll") == 0)
	{
		download_by_epoll(argv[3],atoi(argv[4]));
		sleep(100000000);	
		return 0;
	}
	if(false)
	{
		download_file_by_filename(NULL);
	}
	return 0;
}
