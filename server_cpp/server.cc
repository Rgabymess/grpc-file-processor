#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/wait.h>

#include <grpcpp/grpcpp.h>
#include "file_processor.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReaderWriter;
using grpc::Status;
using namespace file_processor;

class FileProcessorServiceImpl final : public FileProcessorService::Service {
private:
    std::mutex log_mutex_;

    // ==============================================================================
    // IMPLEMENTAÇÃO SUA (MÉTODOS AUXILIARES)
    // ==============================================================================
    void LogOperation(const std::string& type, const std::string& service,
                      const std::string& file, const std::string& msg) {
        std::lock_guard<std::mutex> lock(log_mutex_);  // ctime não é thread-safe

        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::string timestamp = std::ctime(&t);
        if (!timestamp.empty() && timestamp.back() == '\n') timestamp.pop_back();

        std::ofstream log("server.log", std::ios::app);
        if (log) {
            log << "[" << timestamp << "] "
                << "[" << type << "] "
                << "servico=" << service << " "
                << "arquivo=" << file << " "
                << "msg=" << msg << std::endl;
        }
        // o ofstream fecha o arquivo ao sair do escopo
    }

    // Executa um comando, captura stdout+stderr e devolve o código de retorno
    int RunCommand(const std::string& cmd, std::string& output) {
        output.clear();
        FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
        if (!pipe) {
            output = "Falha ao executar o comando";
            return -1;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        int status = pclose(pipe);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // Aceita só letras, números, '.', '_' e '-' (evita injeção de comando / path traversal)
    bool IsSafeName(const std::string& name) {
        if (name.empty() || name.find("..") != std::string::npos) return false;
        for (char c : name) {
            if (!(isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'))
                return false;
        }
        return true;
    }

    // ==============================================================================
    // MÉTODOS AUXILIARES FORNECIDOS (NÃO ALTERAR)
    // ==============================================================================
    bool ReceiveFile(ServerReaderWriter<FileResponse, FileRequest>* stream, std::string& filepath, std::string& original_name, FileRequest& last_req) {
        FileRequest req;
        std::ofstream outfile;
        bool first = true;
        while (stream->Read(&req)) {
            if (first) {
                original_name = req.file_name();
                filepath = "/tmp/input_" + original_name;
                outfile.open(filepath, std::ios::binary);
                last_req = req;
                first = false;
            }
            if (req.has_file_content()) {
                outfile.write(req.file_content().content().c_str(), req.file_content().content().size());
            }
        }
        if (outfile.is_open()) outfile.close();
        return !first;
    }

    Status SendFile(ServerReaderWriter<FileResponse, FileRequest>* stream, const std::string& out_filepath, const std::string& out_filename) {
        std::ifstream infile(out_filepath, std::ios::binary);
        if (!infile) return Status(grpc::StatusCode::INTERNAL, "Falha ao ler output");
        FileResponse resp;
        resp.set_success(true);
        resp.set_file_name(out_filename);
        resp.set_status_message("Sucesso");
        char buffer[1024];
        while (infile.read(buffer, sizeof(buffer)) || infile.gcount() > 0) {
            FileChunk* chunk = new FileChunk();
            chunk->set_content(buffer, infile.gcount());
            resp.set_allocated_file_content(chunk);
            stream->Write(resp);
            resp.clear_file_content();
        }
        return Status::OK;
    }

public:
    // ==============================================================================
    // IMPLEMENTAÇÃO SUA (SERVIÇOS gRPC)
    // ==============================================================================
    Status CompressPDF(ServerContext* context, ServerReaderWriter<FileResponse, FileRequest>* stream) override {
        const std::string service = "CompressPDF";
        std::string in_path, original_name;
        FileRequest first_req;

        // 1. Recebe o arquivo do cliente
        if (!ReceiveFile(stream, in_path, original_name, first_req)) {
            LogOperation("ERRO", service, "-", "Nenhum arquivo recebido");
            return Status(grpc::StatusCode::INVALID_ARGUMENT, "Nenhum arquivo recebido");
        }
        LogOperation("INFO", service, original_name, "Arquivo recebido");

        if (!IsSafeName(original_name)) {
            std::remove(in_path.c_str());
            LogOperation("ERRO", service, original_name, "Nome de arquivo invalido");
            return Status(grpc::StatusCode::INVALID_ARGUMENT, "Nome de arquivo invalido");
        }

        // 2. Monta o comando do Ghostscript
        const std::string out_name = "comprimido_" + original_name;
        const std::string out_path = "/tmp/output_" + original_name;
        const std::string cmd =
            "gs -sDEVICE=pdfwrite -dCompatibilityLevel=1.4 -dPDFSETTINGS=/ebook "
            "-dNOPAUSE -dQUIET -dBATCH -sOutputFile=" + out_path + " " + in_path;

        // 3. Executa o comando
        std::string cmd_output;
        int rc = RunCommand(cmd, cmd_output);

        // 4. Registra o resultado
        if (rc != 0) {
            LogOperation("ERRO", service, original_name,
                         "Ghostscript falhou (codigo " + std::to_string(rc) + "): " + cmd_output);
            std::remove(in_path.c_str());
            std::remove(out_path.c_str());
            return Status(grpc::StatusCode::INTERNAL, "Falha ao comprimir o PDF: " + cmd_output);
        }
        LogOperation("SUCESSO", service, original_name, "PDF comprimido com sucesso");

        // 5. Devolve o arquivo comprimido
        Status status = SendFile(stream, out_path, out_name);
        if (!status.ok()) {
            LogOperation("ERRO", service, original_name, "Falha ao enviar arquivo: " + status.error_message());
        }

        // 6. Remove os temporários
        std::remove(in_path.c_str());
        std::remove(out_path.c_str());
        return status;
    }

    // ==============================================================================
    // MÉTODOS IGNORADOS PARA ESTA ENTREGA
    // ==============================================================================
    Status ConvertToTXT(ServerContext* /*context*/, ServerReaderWriter<FileResponse, FileRequest>* /*stream*/) override {
        return Status::OK;
    }

    Status ConvertImageFormat(ServerContext* /*context*/, ServerReaderWriter<FileResponse, FileRequest>* /*stream*/) override {
        return Status::OK;
    }

    Status ResizeImage(ServerContext* /*context*/, ServerReaderWriter<FileResponse, FileRequest>* /*stream*/) override {
        return Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    FileProcessorServiceImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Servidor gRPC operacional em " << server_address << std::endl;
    server->Wait();
}

int main() {
    RunServer();
    return 0;
}
