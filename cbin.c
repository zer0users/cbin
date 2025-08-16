#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>

#define MAX_LINE_LENGTH 1024
#define MAX_FILES 1000
#define MAX_COMMANDS 100

typedef struct {
    char path[512];
    char *data;
    size_t size;
} EmbeddedFile;

typedef struct {
    char command[512];
} Command;

typedef struct {
    EmbeddedFile files[MAX_FILES];
    int file_count;
    Command commands[MAX_COMMANDS];
    int command_count;
    char project_name[256];
} CBinProject;

// Función para leer archivo completo
char* read_file(const char* filename, size_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) return NULL;
    
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* buffer = malloc(*size);
    if (buffer) {
        fread(buffer, 1, *size, file);
    }
    fclose(file);
    return buffer;
}

// Función para verificar si un archivo debe ser excluido
int should_exclude_file(const char* path) {
    const char* filename = strrchr(path, '/');
    if (filename) filename++; else filename = path;
    
    // Excluir binario cbin y archivos temporales
    if (strcmp(filename, "cbin") == 0) return 1;
    if (strstr(filename, "_packaged.c")) return 1;
    if (strstr(filename, ".o")) return 1;
    
    return 0;
}

// Función recursiva para incluir archivos y carpetas
void include_directory(CBinProject* project, const char* path, const char* base_path) {
    DIR* dir = opendir(path);
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && project->file_count < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // Recursión en subdirectorios
                include_directory(project, full_path, base_path);
            } else if (S_ISREG(st.st_mode)) {
                // Verificar si debe ser excluido
                if (should_exclude_file(full_path)) {
                    continue;
                }
                
                // Incluir archivo
                size_t size;
                char* data = read_file(full_path, &size);
                if (data) {
                    EmbeddedFile* file = &project->files[project->file_count];
                    
                    // Usar ruta relativa
                    if (strncmp(full_path, base_path, strlen(base_path)) == 0) {
                        strcpy(file->path, full_path + strlen(base_path) + 1);
                    } else {
                        strcpy(file->path, full_path);
                    }
                    
                    file->data = data;
                    file->size = size;
                    project->file_count++;
                }
            }
        }
    }
    closedir(dir);
}

// Parser simple para Runfile
void parse_runfile(CBinProject* project, const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Error: No se encontró Runfile\n");
        exit(1);
    }
    
    char line[MAX_LINE_LENGTH];
    
    while (fgets(line, sizeof(line), file) && project->command_count < MAX_COMMANDS) {
        // Remover salto de línea
        line[strcspn(line, "\n")] = 0;
        
        // Ignorar líneas vacías y comentarios
        if (strlen(line) == 0 || line[0] == '/' && line[1] == '/') {
            continue;
        }
        
        // Agregar comando
        strcpy(project->commands[project->command_count].command, line);
        project->command_count++;
    }
    
    fclose(file);
}

// Función para crear directorios recursivamente
void create_directories_for_path(const char* filepath) {
    char* path_copy = strdup(filepath);
    char* dir_path = dirname(path_copy);
    
    if (strcmp(dir_path, ".") != 0 && strcmp(dir_path, "/") != 0) {
        struct stat st = {0};
        if (stat(dir_path, &st) == -1) {
            create_directories_for_path(dir_path);
            mkdir(dir_path, 0755);
        }
    }
    free(path_copy);
}

// Generar código C autónomo
void generate_executable_code(CBinProject* project, const char* output_name) {
    char c_filename[512];
    snprintf(c_filename, sizeof(c_filename), "%s_packaged.c", output_name);
    
    FILE* c_file = fopen(c_filename, "w");
    if (!c_file) {
        printf("Error creando archivo C\n");
        exit(1);
    }
    
    fprintf(c_file, "#include <stdio.h>\n");
    fprintf(c_file, "#include <stdlib.h>\n");
    fprintf(c_file, "#include <string.h>\n");
    fprintf(c_file, "#include <sys/stat.h>\n");
    fprintf(c_file, "#include <unistd.h>\n");
    fprintf(c_file, "#include <libgen.h>\n\n");
    
    // Función para crear directorios
    fprintf(c_file, "void create_dirs(const char* filepath) {\n");
    fprintf(c_file, "    char* path_copy = strdup(filepath);\n");
    fprintf(c_file, "    char* dir_path = dirname(path_copy);\n");
    fprintf(c_file, "    if (strcmp(dir_path, \".\") != 0 && strcmp(dir_path, \"/\") != 0) {\n");
    fprintf(c_file, "        struct stat st = {0};\n");
    fprintf(c_file, "        if (stat(dir_path, &st) == -1) {\n");
    fprintf(c_file, "            create_dirs(dir_path);\n");
    fprintf(c_file, "            mkdir(dir_path, 0755);\n");
    fprintf(c_file, "        }\n");
    fprintf(c_file, "    }\n");
    fprintf(c_file, "    free(path_copy);\n");
    fprintf(c_file, "}\n\n");
    
    // Datos embebidos
    fprintf(c_file, "// Archivos embebidos\n");
    for (int i = 0; i < project->file_count; i++) {
        fprintf(c_file, "static const unsigned char file_%d_data[] = {\n", i);
        
        EmbeddedFile* file = &project->files[i];
        for (size_t j = 0; j < file->size; j++) {
            if (j % 16 == 0) fprintf(c_file, "    ");
            fprintf(c_file, "0x%02x", (unsigned char)file->data[j]);
            if (j < file->size - 1) fprintf(c_file, ",");
            if (j % 16 == 15 || j == file->size - 1) fprintf(c_file, "\n");
        }
        
        fprintf(c_file, "};\n");
        fprintf(c_file, "static const char file_%d_path[] = \"%s\";\n", i, file->path);
        fprintf(c_file, "static const size_t file_%d_size = %zu;\n\n", i, file->size);
    }
    
    // Comandos embebidos
    fprintf(c_file, "// Comandos a ejecutar\n");
    fprintf(c_file, "static const char* commands[] = {\n");
    for (int i = 0; i < project->command_count; i++) {
        fprintf(c_file, "    \"");
        // Escapar caracteres especiales en el comando
        for (int j = 0; project->commands[i].command[j]; j++) {
            char c = project->commands[i].command[j];
            if (c == '"') {
                fprintf(c_file, "\\\"");
            } else if (c == '\\') {
                fprintf(c_file, "\\\\");
            } else if (c == '\n') {
                fprintf(c_file, "\\n");
            } else if (c == '\t') {
                fprintf(c_file, "\\t");
            } else {
                fprintf(c_file, "%c", c);
            }
        }
        fprintf(c_file, "\"");
        if (i < project->command_count - 1) fprintf(c_file, ",");
        fprintf(c_file, "\n");
    }
    fprintf(c_file, "};\n");
    fprintf(c_file, "static const int command_count = %d;\n\n", project->command_count);
    
    // Función principal
    fprintf(c_file, "int main() {\n");
    fprintf(c_file, "    char temp_dir[256];\n");
    fprintf(c_file, "    snprintf(temp_dir, sizeof(temp_dir), \"/tmp/%s_XXXXXX\", \"%s\");\n", project->project_name, project->project_name);
    fprintf(c_file, "    \n");
    fprintf(c_file, "    // Crear directorio temporal único\n");
    fprintf(c_file, "    if (!mkdtemp(temp_dir)) {\n");
    fprintf(c_file, "        return 1;\n");
    fprintf(c_file, "    }\n");
    fprintf(c_file, "    \n");
    fprintf(c_file, "    // Cambiar al directorio temporal\n");
    fprintf(c_file, "    char original_dir[1024];\n");
    fprintf(c_file, "    getcwd(original_dir, sizeof(original_dir));\n");
    fprintf(c_file, "    chdir(temp_dir);\n");
    fprintf(c_file, "    \n");
    
    // Extraer TODOS los archivos embebidos
    fprintf(c_file, "    // Extraer todos los archivos embebidos\n");
    for (int i = 0; i < project->file_count; i++) {
        fprintf(c_file, "    // Extraer: %s\n", project->files[i].path);
        fprintf(c_file, "    create_dirs(file_%d_path);\n", i);
        fprintf(c_file, "    {\n");
        fprintf(c_file, "        FILE* f = fopen(file_%d_path, \"wb\");\n", i);
        fprintf(c_file, "        if (f) {\n");
        fprintf(c_file, "            fwrite(file_%d_data, 1, file_%d_size, f);\n", i, i);
        fprintf(c_file, "            fclose(f);\n");
        fprintf(c_file, "        }\n");
        fprintf(c_file, "    }\n\n");
    }
    
    // Hacer ejecutables los archivos que lo requieran
    fprintf(c_file, "    // Hacer ejecutables los archivos necesarios\n");
    for (int i = 0; i < project->file_count; i++) {
        if (strstr(project->files[i].path, "bin/") || 
            strstr(project->files[i].path, ".py") ||
            strstr(project->files[i].path, ".sh")) {
            fprintf(c_file, "    chmod(file_%d_path, 0755);\n", i);
        }
    }
    fprintf(c_file, "\n");
    
    // Ejecutar comandos del Runfile
    fprintf(c_file, "    // Ejecutar comandos del Runfile\n");
    fprintf(c_file, "    int exit_code = 0;\n");
    fprintf(c_file, "    for (int i = 0; i < command_count; i++) {\n");
    fprintf(c_file, "        int result = system(commands[i]);\n");
    fprintf(c_file, "        if (result != 0) {\n");
    fprintf(c_file, "            exit_code = 1;\n");
    fprintf(c_file, "            break;\n");
    fprintf(c_file, "        }\n");
    fprintf(c_file, "    }\n\n");
    
    // Limpiar directorio temporal
    fprintf(c_file, "    // Regresar al directorio original\n");
    fprintf(c_file, "    chdir(original_dir);\n");
    fprintf(c_file, "    \n");
    fprintf(c_file, "    // Eliminar directorio temporal recursivamente\n");
    fprintf(c_file, "    char rm_cmd[512];\n");
    fprintf(c_file, "    snprintf(rm_cmd, sizeof(rm_cmd), \"rm -rf %%s\", temp_dir);\n");
    fprintf(c_file, "    system(rm_cmd);\n");
    fprintf(c_file, "    \n");
    fprintf(c_file, "    return exit_code;\n");
    fprintf(c_file, "}\n");
    
    fclose(c_file);
    
    // Compilar con gcc mostrando errores para debug
    char compile_cmd[1024];
    snprintf(compile_cmd, sizeof(compile_cmd), "gcc -O2 -o %s %s", output_name, c_filename);
    
    if (system(compile_cmd) == 0) {
        // Limpiar archivo temporal
        unlink(c_filename);
    } else {
        printf("Error compilando. Revisa el archivo %s para ver el código generado.\n", c_filename);
        exit(1);
    }
}

int main() {
    CBinProject project = {0};
    
    // Obtener nombre del directorio actual como nombre del proyecto
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        char* project_name = strrchr(cwd, '/');
        if (project_name) {
            strcpy(project.project_name, project_name + 1);
        } else {
            strcpy(project.project_name, cwd);
        }
    } else {
        strcpy(project.project_name, "proyecto");
    }
    
    // Incluir todos los archivos del directorio actual
    include_directory(&project, ".", ".");
    
    // Parsear Runfile
    parse_runfile(&project, "Runfile");
    
    // Generar ejecutable que contiene todo
    generate_executable_code(&project, project.project_name);
    
    // Liberar memoria
    for (int i = 0; i < project.file_count; i++) {
        free(project.files[i].data);
    }
    
    return 0;
}
