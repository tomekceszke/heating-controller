package com.ceszke.heatingcontroller;

import com.google.cloud.bigquery.*;
import com.google.cloud.functions.HttpFunction;
import com.google.cloud.functions.HttpRequest;
import com.google.cloud.functions.HttpResponse;
import com.google.gson.Gson;
import com.google.gson.reflect.TypeToken;

import java.io.IOException;
import java.util.List;
import java.util.Map;

import static com.google.api.client.http.HttpStatusCodes.*;

public class MetricsSender implements HttpFunction {

    private static final Gson gson = new Gson();

    @Override
    public void service(HttpRequest request, HttpResponse response) throws IOException {
        InsertAllRequest.RowToInsert rowToInsert = getRowToInsert(request);
        BigQuery bigquery = BigQueryOptions.getDefaultInstance().getService();
        TableId tableId = TableId.of(System.getenv("DATASET"), System.getenv("TABLE"));
        try {
            InsertAllResponse dbResponse =
                    bigquery.insertAll(
                            InsertAllRequest.newBuilder(tableId)
                                    .addRow(rowToInsert)
                                    .build());
            if (dbResponse.hasErrors()) {
                response.setStatusCode(STATUS_CODE_BAD_REQUEST, "BAD REQUEST");
                for (Map.Entry<Long, List<BigQueryError>> entry : dbResponse.getInsertErrors().entrySet()) {
                    System.err.println("Response error: \n" + entry.getValue());
                }
            } else {
                response.setStatusCode(STATUS_CODE_CREATED, "CREATED");
                System.out.println("Rows successfully inserted into table");
            }
        } catch (BigQueryException e) {
            System.err.println("Insert operation not performed \n" + e);
            response.setStatusCode(STATUS_CODE_SERVER_ERROR, "DB ERROR");

            System.out.println(e);

        }
        //        var writer = new PrintWriter(response.getWriter());
        //        writer.printf("Hello %s!", name);
        //    BufferedWriter writer = response.getWriter();
        //        writer.write("Hello World!");
    }

    static InsertAllRequest.RowToInsert getRowToInsert(HttpRequest request) throws IOException {
        Map<String, String> body = gson.fromJson(request.getReader(), new TypeToken<>() {
        });
//        System.out.println(body);
        return InsertAllRequest.RowToInsert.of(body);
    }
}

