package com.ceszke.heatingcontroller;

import com.google.cloud.bigquery.InsertAllRequest;
import com.google.cloud.functions.HttpRequest;
import com.google.cloud.functions.HttpResponse;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;
import org.mockito.Mock;
import org.mockito.MockitoAnnotations;

import java.io.*;
import java.util.Map;
import java.util.stream.Collectors;

import static com.google.common.truth.Truth.assertThat;
import static org.mockito.Mockito.when;

@RunWith(JUnit4.class)
public class MetricsSenderTest {
    @Mock
    private HttpRequest request;
    @Mock
    private HttpResponse response;

    private BufferedWriter writerOut;
    private StringWriter responseOut;

    @Before
    public void beforeTest() throws IOException {
        MockitoAnnotations.openMocks(this);

        responseOut = new StringWriter();
        writerOut = new BufferedWriter(responseOut);
        when(response.getWriter()).thenReturn(writerOut);
    }

    @Test
    public void senderTest() throws IOException {
        new MetricsSender().service(request, response);



        writerOut.flush();
        assertThat(responseOut.toString()).isNotEmpty();
    }

    @Test
    public void getRowToInsert() throws IOException {
        String content = "{\"consumption\":0,\"start_time\":1691791039,\"stop_time\":1691791040}";
        StringReader reader = new StringReader(content);
        when(request.getReader()).thenReturn(new BufferedReader(reader));
        InsertAllRequest.RowToInsert rowToInsert = MetricsSender.getRowToInsert(request);
        Assert.assertEquals(content.replace("\"", ""), rowToInsert.getContent().entrySet()
                .stream()
                .sorted(Map.Entry.<String, Object>comparingByKey())
                .map(entry -> entry.getKey() + ":" + entry.getValue())
                .collect(Collectors.joining(",","{","}")));
        //System.out.println(rowToInsert.getContent());
    }


}