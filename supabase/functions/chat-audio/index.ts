import { serve } from "https://deno.land/std@0.170.0/http/server.ts";
import OpenAI, { toFile } from "https://deno.land/x/openai@v4.26.0/mod.ts";
import { multiParser } from 'https://deno.land/x/multiparser@0.114.0/mod.ts';
import { corsHeaders } from "../common/cors.ts";


const processAudio = async (req: Request) => {
  
  if (req.method !== "POST") {
    return new Response("Method Not Allowed", { status: 405 });
  }

  const openaiClient = new OpenAI({
    apiKey: Deno.env.get("OPENAI_API_KEY"),
  });

  const contentType = req.headers.get('Content-Type') || '';
    let arrayBuffer: ArrayBuffer;
    let filenameTimestamp = `audio_${Date.now()}.wav`;

    if (contentType.includes('multipart/form-data')) {
        const form = await multiParser(req);
        if (!form || !form.files || !form.files.file) {
            return new Response('File not found in form', {
                status: 400,
                headers: corsHeaders,
            });
        }
        console.log('Form:', form);
        const file = form.files.file;
        arrayBuffer = file.content.buffer;
        filenameTimestamp = file.filename || filenameTimestamp;
    } else {
        arrayBuffer = await req.arrayBuffer();
    }

  let transcript: string;
  try {
    const filenameTimestamp = `adeus_wav_${Date.now()}.wav`;
    const wavFile = await toFile(arrayBuffer, filenameTimestamp);

    const transcriptResponse = await openaiClient.audio.transcriptions.create({
      file: await toFile(wavFile, filenameTimestamp),
      model: "whisper-1",
      prompt:
        'Listen to the entire audio file, if no audio is detected then respond with "None" ', // These types of prompts dont work well with Whisper -- https://platform.openai.com/docs/guides/speech-to-text/prompting
    });
    transcript = transcriptResponse.text;
    let transcriptLowered = transcript.toLowerCase();

    if (
      transcript == "None" ||
      transcript == "" ||
      transcript == null ||
      (transcriptLowered.includes("thank") &&
        transcriptLowered.includes("watch"))
    ) {
      return new Response(JSON.stringify({ message: "No transcript found." }), {
        headers: { ...corsHeaders, "Content-Type": "application/json" },
        status: 403,
      });
    }

    console.log("Transcript:", transcript);
  

    const response = await openaiClient.chat.completions.create({
      model: 'gpt-3.5-turbo',
      messages: [
          {
              role: 'system',
              content: `
I want you to act as a spoken English teacher and improver. I will speak to you in English and you will reply to me in English to practice my spoken English. 
              I want you to keep your reply neat, limiting the reply to 100 words. I want you to strictly correct my grammar mistakes, typos, and factual errors. 
              I want you to ask me a question in your reply. Now let's start practicing, you could ask me a question first. Remember,
              I want you to strictly correct my grammar mistakes, typos, and factual errors.`,
          },
          {
              role: 'user',
              content: `${transcript}`,
          },
      ]
    });

    console.log("Teacher:", response);
    const responseData = response.choices[0].message.content;
    console.log("Reply:", responseData);

    const mp3 = await openaiClient.audio.speech.create({
      model: "tts-1",
      voice: "alloy",
      input: responseData,
      response_format: "wav"
    });
    const buffer = await mp3.arrayBuffer();
    return new Response(buffer, {
      headers: { ...corsHeaders, "Content-Type": "application/octet-stream" },
      status: 200,
    });


  } catch (error) {
    console.error("Transcription error:", error);
    return new Response(JSON.stringify({ error: error.message }), {
      headers: { ...corsHeaders, "Content-Type": "application/json" },
      status: 500,
    });
  }
};

serve(processAudio);